/*
 * Dibbler - a portable DHCPv6
 *
 * authors: Tomasz Mrugalski <thomson@klub.com.pl>
 *          Marek Senderski <msend@o2.pl>
 * changes: Krzysztof Wnuk <keczi@poczta.onet.pl>
 *          Michal Kowalczuk <michal@kowalczuk.eu>
 *
 * released under GNU GPL v2 only licence
 *
 * $Id: ClntMsgRequest.cpp,v 1.29 2009-03-24 23:17:17 thomson Exp $
 *
 */

#include "ClntMsgRequest.h"
#include "SmartPtr.h"
#include "DHCPConst.h"
#include "ClntIfaceMgr.h"
#include "ClntMsgAdvertise.h"
#include "ClntOptServerIdentifier.h"
#include "ClntOptServerUnicast.h"
#include "ClntOptIA_NA.h"
#include "ClntOptTA.h"
#include "ClntOptIA_PD.h"
#include "ClntOptElapsed.h"
#include "ClntOptClientIdentifier.h"
#include "ClntOptOptionRequest.h"
#include "ClntOptStatusCode.h"
#include "ClntTransMgr.h"
#include <cmath>
#include "Logger.h"

/*
 * opts - options list WITHOUT serverDUID
 */
TClntMsgRequest::TClntMsgRequest(List(TOpt) opts, int iface)
    :TClntMsg(iface, 0, REQUEST_MSG) {
    IRT = REQ_TIMEOUT;
    MRT = REQ_MAX_RT;
    MRC = REQ_MAX_RC;
    MRD = 0;   
    RT=0;

    Iface=iface;
    IsDone=false;

    int backupCount = ClntTransMgr().getAdvertiseLstCount();
    if (!backupCount) 
    {
	Log(Error) << "Unable to send REQUEST. There are no backup servers left." << LogEnd;
	setState( opts, STATE_NOTCONFIGURED);

        this->IsDone = true;
        return;
    }
    Log(Info) << "Creating REQUEST. Backup server list contains " 
	      << backupCount << " server(s)." << LogEnd;
    ClntTransMgr().printAdvertiseLst();

    // get server DUID from the first advertise
    SPtr<TOpt> srvDUID = ClntTransMgr().getAdvertiseDUID();

    ClntTransMgr().firstAdvertise();
    SPtr<TClntMsgAdvertise> advertise = (Ptr*) ClntTransMgr().getAdvertise();
    this->copyAAASPI((SPtr<TClntMsg>)advertise);

    // remove just used server
    ClntTransMgr().delFirstAdvertise();

    // copy whole list from SOLICIT ...
    Options = opts;
    // set proper Parent in copied options
    Options.first();
    SPtr<TOpt> opt;
    while (opt = Options.get()) {
        // delete OPTION_AAAAUTH (needed only in SOLICIT) and we will append a new elapsed time option later
        if (opt->getOptType() == OPTION_AAAAUTH || opt->getOptType() == OPTION_ELAPSED_TIME)
            Options.del();
        else
            opt->setParent(this);
    }

    // delete OPTION_KEYGEN from OPTION_ORO (needed only once)
    /// @todo: shouldn't it be done when receiving OPTION_KEYGEN in ADVERTISE?
    SPtr<TClntOptOptionRequest> optORO = (Ptr*) this->getOption(OPTION_ORO);
    if (optORO) {
        if (advertise->getOption(OPTION_KEYGEN))
            optORO->delOption(OPTION_KEYGEN);
        // delete also OPTION_AUTH from OPTION_ORO
        /// @todo: shouldn't it be done when receiving OPTION_AUTH in ADVERTISE?
        if (advertise->getOption(OPTION_AUTH))
            optORO->delOption(OPTION_AUTH);
    }

    // copy addresses offered in ADVERTISE
    copyAddrsFromAdvertise((Ptr*) advertise);

    // does this server support unicast?
    SPtr<TClntCfgIface> cfgIface = ClntCfgMgr().getIface(iface);
    if (!cfgIface) {
	Log(Error) << "Unable to find interface with ifindex " << iface << "." << LogEnd;    
	IsDone = true;
	return;
    }
    SPtr<TClntOptServerUnicast> unicast = (Ptr*) advertise->getOption(OPTION_UNICAST);
    if (unicast && !cfgIface->getUnicast()) {
	Log(Info) << "Server offers unicast (" << *unicast->getAddr() 
		  << ") communication, but this client is not configured to so." << LogEnd;
    }
    if (unicast && cfgIface->getUnicast()) {
	Log(Debug) << "Server supports unicast on address " << *unicast->getAddr() << "." << LogEnd;
	//this->PeerAddr = unicast->getAddr();
	Options.first();
	SPtr<TOpt> opt;
	// set this unicast address in each IA in AddrMgr
	while (opt = Options.get()) {
	    if (opt->getOptType()!=OPTION_IA_NA)
		continue;
	    SPtr<TClntOptIA_NA> ptrOptIA = (Ptr*) opt;
	    SPtr<TAddrIA> ptrAddrIA = ClntAddrMgr().getIA(ptrOptIA->getIAID());
	  
	    if (!ptrAddrIA) {
		Log(Crit) << "IA with IAID=" << ptrOptIA->getIAID() << " not found." << LogEnd;
		continue;
	    }
	    ptrAddrIA->setUnicast(unicast->getAddr());
	}
	
    }
    
    // ... and append server's DUID from ADVERTISE
    Options.append( srvDUID );
    
    appendElapsedOption();
    appendAuthenticationOption();
    IsDone = false;
    send();
}

TClntMsgRequest::TClntMsgRequest(List(TAddrIA) IAs,
				 SPtr<TDUID> srvDUID, int iface)
    :TClntMsg(iface, 0, REQUEST_MSG) {
    IRT = REQ_TIMEOUT;
    MRT = REQ_MAX_RT;
    MRC = REQ_MAX_RC;
    MRD = 0;   
    RT=0;

    Iface=iface;
    IsDone=false;
    SPtr<TOpt> ptr;
    ptr = new TClntOptClientIdentifier( ClntCfgMgr().getDUID(), this );
    Options.append( ptr );

    if (!srvDUID) {
	Log(Error) << "Unable to send REQUEST: ServerId not specified.\n" << LogEnd;
	IsDone = true;
	return;
    }
    
    ptr = (Ptr*) SPtr<TClntOptServerIdentifier> (new TClntOptServerIdentifier(srvDUID,this));
    // all IAs provided by checkSolicit
    SPtr<TAddrIA> ClntAddrIA;
    Options.append( ptr );
	
    IAs.first();
    while (ClntAddrIA = IAs.get()) 
    {
        SPtr<TClntCfgIA> ClntCfgIA = ClntCfgMgr().getIA(ClntAddrIA->getIAID());
        SPtr<TClntOptIA_NA> IA_NA = new TClntOptIA_NA(ClntCfgIA, ClntAddrIA, this);
        Options.append((Ptr*)IA_NA);
    }

    appendElapsedOption();
    appendAuthenticationOption();

    pkt = new char[getSize()];
    this->IsDone = false;
    this->send();
}

/*
 * analyse REPLY received for this REQUEST
 */
void TClntMsgRequest::answer(SPtr<TClntMsg> msg)
{
    this->copyAAASPI(msg);

#ifdef MOD_CLNT_CONFIRM    
    /* CHANGED here: When the client receives a NotOnLink status from the server in
     * response to a Request, the client can either re-issue the Request
     * without specifying any addresses or restart the DHCP server discovery
     * process.
     */  
    SPtr<TOptStatusCode> optStateCode = (Ptr*)msg->getOption(OPTION_STATUS_CODE);
    if( optStateCode && STATUSCODE_NOTONLINK == optStateCode->getCode()){
	SPtr<TOpt> opt;
	Options.first();
	while(opt = Options.get()){
	    if(opt->getOptType() != OPTION_IA_NA){
		continue;
	    }
	    SPtr<TClntOptIA_NA> IA_NA = (Ptr*)opt;
	    IA_NA->firstOption();
	    SPtr<TOpt> subOpt;
	    while( subOpt = IA_NA->getOption()){
		if(subOpt->getOptType() == OPTION_IAADDR)
		    IA_NA->delOption();
	    }
	} 
	return;   
    }
#endif
     
    TClntMsg::answer(msg);
}

void TClntMsgRequest::doDuties()
{
    // timeout is reached and we still don't have answer, retransmit
    if (RC>MRC) 
    {
        ClntTransMgr().sendRequest(Options, Iface);

        IsDone = true;
        return;
    }
    send();
    return;
}

bool TClntMsgRequest::check()
{
    return false;
}

string TClntMsgRequest::getName() {
    return "REQUEST";
}

/** 
 * method is used when REQUEST transmission was attempted, but there are no more servers
 * on the backup list. When this method is called, it is sure that some IAs, TA or PDs will
 * remain not configured (as there are no servers available, which could configure it)
 * 
 * @param options list of still unconfigured options 
 * @param state state to be set in CfgMgr
 */
void TClntMsgRequest::setState(List(TOpt) options, EState state)
{
    SPtr<TOpt>          opt;
    SPtr<TClntOptIA_NA> ia;
    SPtr<TClntOptTA>    ta;
    SPtr<TClntOptIA_PD> pd;

    SPtr<TClntCfgIA> cfgIa;
    SPtr<TClntCfgTA> cfgTa;
    SPtr<TClntCfgPD> cfgPd;

    SPtr<TClntCfgIface> iface = ClntCfgMgr().getIface(Iface);
    if (!iface) {
	      Log(Error) << "Unable to find interface with ifindex=" << Iface << LogEnd;
	      return;
    }

    options.first();
    while ( opt = options.get() ) {
	      switch (opt->getOptType()) {
	      case OPTION_IA_NA:
	      {
	          ia = (Ptr*) opt;
	          cfgIa = iface->getIA(ia->getIAID());
	          if (cfgIa)
		      cfgIa->setState(state);
	          break;
	      }
	      case OPTION_IA_TA:
	      {
	          ia = (Ptr*) opt;
	          iface->firstTA();
	          cfgTa = iface->getTA();
	          if (cfgTa)
		      cfgTa->setState(state);
	          break;
	      }
	      case OPTION_IA_PD:
	      {
	          pd = (Ptr*) opt;
	          cfgPd = iface->getPD(pd->getIAID());
	          if (cfgPd)
		      cfgPd->setState(state);
	          break;
	      }
      	default:
	        continue;
	    }
    }
}

void TClntMsgRequest::copyAddrsFromAdvertise(SPtr<TClntMsg> adv)
{
    SPtr<TOpt> opt1, opt2, opt3;
    SPtr<TClntOptIA_NA> ia1, ia2;

    this->copyAAASPI(adv);

    Options.first();
    while (opt1 = Options.get()) {
	if (opt1->getOptType()!=OPTION_IA_NA)
	    continue; // ignore all options except IA_NA
	adv->firstOption();
	while (opt2 = adv->getOption()) {
	    if (opt2->getOptType() != OPTION_IA_NA)
		continue;
	    ia1 = (Ptr*) opt1;
	    ia2 = (Ptr*) opt2;
	    if (ia1->getIAID() != ia2->getIAID())
		continue;

	    // found IA in ADVERTISE, now copy all addrs

	    ia1->delAllOptions();
	    ia2->firstOption();
	    while (opt3 = ia2->getOption()) {
		if (opt3->getOptType() == OPTION_IAADDR)
		    ia1->addOption(opt3);
	    }

	}
    }
    

}

TClntMsgRequest::~TClntMsgRequest() {
}

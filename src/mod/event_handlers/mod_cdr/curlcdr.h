/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application Call Detail Recorder module
 * Copyright 2006, Author: Yossi Neiman of Cartis Solutions, Inc. <freeswitch AT cartissolutions.com>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application Call Detail Recorder module
 *
 * This code is largely derived from csvcdr.cpp with minor code snippets from mod_xml_curl.c and edited by 
 * Bret McDanel <trixter AT 0xdecafbad.com>
 * The Initial Developer of the Original Code is
 * Yossi Neiman <freeswitch AT cartissolutions.com>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 * 
 * Yossi Neiman <freeswitch AT cartissolutions.com>
 * Bret McDanel <trixter AT 0xdecafbad.com>
 * Anthony Minessale II <anthmct@yahoo.com>
 *
 * Description: This C++ source file describes the CurlCDR class that handles processing CDRs to HTTP endpoint.
 * This is the standard Curl module, and has a list of predefined variables to log out which can be
 * added to, but never have the default ones removed.  If you want to use one that allows you to explicity
 * set all data variables to be logged and in what order, then this is not the class you want to use, and
 * one will be coming in the future to do just that.
 *
 * curlcdr.h
 *
 */

#include "baseregistry.h"
#include <switch.h>
#include <iostream>
#include <fstream>
#include <list>
#include <sstream>


#ifndef CURLCDR
#define CURLCDR

class CurlCDR:public BaseCDR {
  public:
	CurlCDR();
	CurlCDR(switch_mod_cdr_newchannel_t * newchannel);
	//CurlCDR(const CurlCDR& copyFrom);
	virtual ~ CurlCDR();
	virtual bool process_record();
	virtual void connect(switch_xml_t & cfg, switch_xml_t & xml, switch_xml_t & settings, switch_xml_t & param);	// connect and disconnect need to be static because we're persisting connections until shutdown
	virtual void disconnect();
	virtual bool is_activated();
	virtual void tempdump_record();
	virtual void reread_tempdumped_records();
	virtual std::string get_display_name();
	virtual std::string itos(int i);
	virtual std::string lltos(long long ll);

  private:
	static bool activated;		// Is this module activated?
	static bool connectionstate;	// What is the status of the connection?
	static bool logchanvars;
	static modcdr_time_convert_t convert_time;
	static const char *gateway_url;	// The URL to send data to
	static const char *gateway_credentials;	// The credentials for http auth
	static std::list < std::string > chanvars_fixed_list;	// Normally this would be used, but not in this class
	static std::list < std::string > chanvars_supp_list;	// This will hold the list for all chanvars here
	static std::string display_name;
	static std::string postdata;
	char formattedcallstartdate[100];
	char formattedcallanswerdate[100];
	char formattedcalltransferdate[100];
	char formattedcallenddate[100];
};

#endif

/* For Emacs:
 * Local Variables:
 * mode:c++
 * indent-tabs-mode:nil
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 expandtab:
 */

/*
 * Copyright (c) 2016 Dell Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 * THIS CODE IS PROVIDED ON AN *AS IS* BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT
 * LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS
 * FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.
 *
 * See the Apache Version 2.0 License for specific language governing
 * permissions and limitations under the License.
 */

/*
 * nas_os_stp.c
 *
 *  Created on: May 19, 2015
 */


#include "dell-base-stg.h"
#include "event_log.h"
#include "std_error_codes.h"
#include "nas_nlmsg.h"
#include "netlink_tools.h"
#include "nas_os_vlan_utils.h"
#include "nas_os_if_priv.h"
#include "nas_os_if_conversion_utils.h"

#include <netinet/in.h>
#include <linux/if_bridge.h>
#include <sys/socket.h>

#include <map>
#include <mutex>

static std::mutex _if_stp_mutex;
static std::map<hal_ifindex_t,uint8_t> _if_stp_state;
#define NL_MSG_BUFF_LEN 4096

extern "C"{

t_std_error get_if_stp_state(hal_ifindex_t index, uint8_t * state){
    std::lock_guard<std::mutex> lock(_if_stp_mutex);
    auto it = _if_stp_state.find(index);
    if(it != _if_stp_state.end()){
        *state = it->second;
        return STD_ERR_OK;
    }
    return STD_ERR(STG,FAIL,0);
}


t_std_error nl_int_update_stp_state(cps_api_object_t obj){
    std::lock_guard<std::mutex> lock(_if_stp_mutex);
    cps_api_object_attr_t ifindex = cps_api_object_attr_get(obj,BASE_STG_ENTRY_INTF_IF_INDEX_IFINDEX);
    cps_api_object_attr_t stp_state = cps_api_object_attr_get(obj,BASE_STG_ENTRY_INTF_STATE);
    cps_api_object_attr_t vlan_id_attr = cps_api_object_attr_get(obj,BASE_STG_ENTRY_VLAN);
    cps_api_object_attr_t os_update_attr = cps_api_object_attr_get(obj,BASE_STG_ENTRY_INTF_IF);
    cps_api_object_attr_t if_name_attr = cps_api_object_attr_get(obj,BASE_STG_ENTRY_INTF_IF_NAME);


    if(ifindex == NULL || stp_state == NULL){
        EV_LOG(ERR,NAS_L2,0,"NAS-LINUX-STG","Ifindex/STP state/VLAN Id Missing for updating STP kernel state");
        return STD_ERR(STG,PARAM,0);
    }

    hal_ifindex_t vlan_ifindex;
    hal_ifindex_t phy_ifindex = cps_api_object_attr_data_u32(ifindex);
    vlan_ifindex = phy_ifindex;

    if(vlan_id_attr != NULL){
        hal_vlan_id_t vlan_id = cps_api_object_attr_data_u32(vlan_id_attr);

        if(if_name_attr){
            const char * intf_name = (const char *)cps_api_object_attr_data_bin(if_name_attr);
            char vlan_intf_name[HAL_IF_NAME_SZ+1];
            snprintf(vlan_intf_name,sizeof(vlan_intf_name)-1,"%s.%d",intf_name,vlan_id);
            get_tagged_intf_index_from_name(vlan_intf_name,vlan_ifindex);
        }
    }

    uint8_t state = cps_api_object_attr_data_u32(stp_state);

    auto state_it = _if_stp_state.find(vlan_ifindex);
    if( state_it != _if_stp_state.end()){
        /*
         * To prevent race condition for eg. port was in disabled and becomes oper up
         * kernel puts it in fwd, at that time this function will be invoked to put it
         * back to disabled. But in the meantime, if stack decides to put port in forwarding
         * there will be race condition so when processing updates from kernel check if
         * state being programmed in the kernel is changed then don't update it
         */
        if(os_update_attr){
            if (state != state_it->second){
                return STD_ERR_OK;
            }
        }else{
            if(state == state_it->second){
                return STD_ERR_OK;
            }
        }

    }

    char buff[NL_MSG_BUFF_LEN];
    memset(buff,0,sizeof(buff));
    struct nlmsghdr *nlh = (struct nlmsghdr *) nlmsg_reserve((struct nlmsghdr *)buff,sizeof(buff),sizeof(struct nlmsghdr));
    struct ifinfomsg *ifmsg = (struct ifinfomsg *) nlmsg_reserve(nlh,sizeof(buff),sizeof(struct ifinfomsg));

    nlh->nlmsg_pid = 0 ;
    nlh->nlmsg_seq = 0 ;
    nlh->nlmsg_flags =  NLM_F_REQUEST | BRIDGE_FLAGS_MASTER |NLM_F_ACK ;
    nlh->nlmsg_type = RTM_SETLINK ;

    ifmsg->ifi_family = AF_BRIDGE;
    ifmsg->ifi_index = vlan_ifindex;

    struct nlattr *stp_attr = nlmsg_nested_start(nlh, sizeof(buff));
    stp_attr->nla_len = 0;
    stp_attr->nla_type = IFLA_PROTINFO | NLA_F_NESTED;
    nlmsg_add_attr(nlh,sizeof(buff),IFLA_BRPORT_STATE,(void *)&state,sizeof(uint8_t));
    nlmsg_nested_end(nlh, stp_attr);

    if(nl_do_set_request(nas_nl_sock_T_INT,nlh, buff, sizeof(buff)) != STD_ERR_OK){
        EV_LOG(ERR,NAS_L2,0,"NAS_LINUX-STG","Failed to updated STP State to %d for Interface %d "
                "in Kernel",state,vlan_ifindex);
        return STD_ERR(STG,FAIL,0);
    }

    _if_stp_state[vlan_ifindex] = state;
    return STD_ERR_OK;
}

}

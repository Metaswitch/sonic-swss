#ifndef SWSS_NEXTHOPKEY_H
#define SWSS_NEXTHOPKEY_H

#include "ipaddress.h"
#include "tokenize.h"
#include "label.h"

#define NH_DELIMITER '@'
#define NHG_DELIMITER ','
#define VRF_PREFIX "Vrf"
extern IntfsOrch *gIntfsOrch;

struct NextHopKey
{
    IpAddress           ip_address;     // neighbor IP address
    string              alias;          // incoming interface alias
    uint32_t            vni;            // Encap VNI overlay nexthop
    MacAddress          mac_address;    // Overlay Nexthop MAC.
    LabelStack          label_stack;    // MPLS label stack

    NextHopKey() = default;
    NextHopKey(const std::string &ipstr, const std::string &alias) : ip_address(ipstr), alias(alias), vni(0), mac_address() {}
    NextHopKey(const IpAddress &ip, const std::string &alias) : ip_address(ip), alias(alias), vni(0), mac_address() {}
    NextHopKey(const std::string &str)
    {
        if (str.find(NHG_DELIMITER) != string::npos)
        {
            std::string err = "Error converting " + str + " to NextHop";
            throw std::invalid_argument(err);
        }
        std::size_t label_delimiter = str.find(LABELSTACK_DELIMITER);
        std::string ip_str;
        if (label_delimiter != std::string::npos)
        {
            label_stack = LabelStack(str.substr(0, label_delimiter));
            ip_str = str.substr(label_delimiter+1);
        }
        else
        {
            ip_str = str;
        }
        auto keys = tokenize(ip_str, NH_DELIMITER);
        vni = 0;
        mac_address = MacAddress();
        if (keys.size() == 1)
        {
            ip_address = keys[0];
            alias = gIntfsOrch->getRouterIntfsAlias(ip_address);
        }
        else if (keys.size() == 2)
        {
            ip_address = keys[0];
            alias = keys[1];
            if (!alias.compare(0, strlen(VRF_PREFIX), VRF_PREFIX))
            {
                alias = gIntfsOrch->getRouterIntfsAlias(ip_address, alias);
            }
        }
        else
        {
            std::string err = "Error converting " + str + " to NextHop";
            throw std::invalid_argument(err);
        }
    }
    NextHopKey(const std::string &str, bool overlay_nh)
    {
        if (str.find(NHG_DELIMITER) != string::npos)
        {
            std::string err = "Error converting " + str + " to NextHop";
            throw std::invalid_argument(err);
        }
        std::size_t label_delimiter = str.find(LABELSTACK_DELIMITER);
        std::string ip_str;
        if (label_delimiter != std::string::npos)
        {
            label_stack = LabelStack(str.substr(0, label_delimiter));
            ip_str = str.substr(label_delimiter+1);
        }
        else
        {
            ip_str = str;
        }
        auto keys = tokenize(ip_str, NH_DELIMITER);
        if (keys.size() != 4)
        {
            std::string err = "Error converting " + str + " to NextHop";
            throw std::invalid_argument(err);
        }
        ip_address = keys[0];
        alias = keys[1];
        vni = static_cast<uint32_t>(std::stoul(keys[2]));
        mac_address = keys[3];
    }

    const std::string to_string() const
    {
        std::string str;
        if (!label_stack.empty())
        {
            str += label_stack.to_string();
            str += LABELSTACK_DELIMITER;
        }
        str += ip_address.to_string() + NH_DELIMITER + alias;
        return str;
    }

    const std::string to_string(bool overlay_nh) const
    {
        std::string str;
        if (!label_stack.empty())
        {
            str += label_stack.to_string();
            str += LABELSTACK_DELIMITER;
        }
        str += (ip_address.to_string() + NH_DELIMITER + alias + NH_DELIMITER +
                std::to_string(vni) + NH_DELIMITER + mac_address.to_string());
        return str;
    }

    bool operator<(const NextHopKey &o) const
    {
        return tie(ip_address, alias, vni, mac_address) < tie(o.ip_address, o.alias, o.vni, o.mac_address);
    }

    bool operator==(const NextHopKey &o) const
    {
        return (ip_address == o.ip_address) && (alias == o.alias) && (vni == o.vni) && (mac_address == o.mac_address);
    }

    bool operator!=(const NextHopKey &o) const
    {
        return !(*this == o);
    }

    bool isIntfNextHop() const
    {
        return (ip_address.getV4Addr() == 0);
    }
};

#endif /* SWSS_NEXTHOPKEY_H */

#ifndef SWSS_NEXTHOPKEY_H
#define SWSS_NEXTHOPKEY_H

#include "ipaddress.h"
#include "tokenize.h"
#include "label.h"
#include "intfsorch.h"

#define LABELSTACK_DELIMITER '+'
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
    sai_outseg_type_t   outseg_type;    // MPLS outseg type

    NextHopKey() = default;
    NextHopKey(const std::string &str, const std::string &alias) :
        alias(alias), vni(0), mac_address(), outseg_type(SAI_OUTSEG_TYPE_SWAP)
    {
        std::string ip_str = parse_mpls(str);
        ip_address = ip_str;
    }
    NextHopKey(const IpAddress &ip, const std::string &alias) :
        ip_address(ip), alias(alias), vni(0), mac_address(), outseg_type(SAI_OUTSEG_TYPE_SWAP) {}
    NextHopKey(const std::string &str) :
        vni(0), mac_address(), outseg_type(SAI_OUTSEG_TYPE_SWAP)
    {
        SWSS_LOG_ENTER();

        if (str.find(NHG_DELIMITER) != string::npos)
        {
            std::string err = "Error converting " + str + " to NextHop";
            throw std::invalid_argument(err);
        }
        std::string ip_str = parse_mpls(str);
        auto keys = tokenize(ip_str, NH_DELIMITER);
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
    NextHopKey(const std::string &str, bool overlay_nh) :
        outseg_type(SAI_OUTSEG_TYPE_SWAP)
    {
        if (str.find(NHG_DELIMITER) != string::npos)
        {
            std::string err = "Error converting " + str + " to NextHop";
            throw std::invalid_argument(err);
        }
        std::string ip_str = parse_mpls(str);
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
        std::string str = format_mpls();
        str += ip_address.to_string() + NH_DELIMITER + alias;
        return str;
    }

    const std::string to_string(bool overlay_nh) const
    {
        std::string str = format_mpls();
        str += (ip_address.to_string() + NH_DELIMITER + alias + NH_DELIMITER +
                std::to_string(vni) + NH_DELIMITER + mac_address.to_string());
        return str;
    }

    bool operator<(const NextHopKey &o) const
    {
        return tie(ip_address, alias, label_stack, outseg_type, vni, mac_address) <
            tie(o.ip_address, o.alias, o.label_stack, o.outseg_type, o.vni, o.mac_address);
    }

    bool operator==(const NextHopKey &o) const
    {
        return (ip_address == o.ip_address) && (alias == o.alias) &&
            (label_stack == o.label_stack) && (outseg_type == o.outseg_type) &&
            (vni == o.vni) && (mac_address == o.mac_address);
    }

    bool operator!=(const NextHopKey &o) const
    {
        return !(*this == o);
    }

    bool isIntfNextHop() const
    {
        return (ip_address.getV4Addr() == 0);
    }

    std::string parse_mpls(const std::string& str)
    {
        std::string ip_str;
        auto keys = tokenize(str, LABELSTACK_DELIMITER);
        if (keys.size() == 1)
        {
            // No MPLS info to parse
            ip_str = str;
        }
        else if (keys.size() == 3)
        {
            if (keys[0] == "swap")
            {
                outseg_type = SAI_OUTSEG_TYPE_SWAP;
            }
            else if (keys[0] == "push")
            {
                outseg_type = SAI_OUTSEG_TYPE_PUSH;
            }
            else
            {
                // Malformed string
                std::string err = "Error converting " + str + " to MPLS NextHop";
                throw std::invalid_argument(err);
            }
            label_stack = LabelStack(keys[1]);
            ip_str = keys[2];
        }
        else
        {
            // Malformed string
            std::string err = "Error converting " + str + " to MPLS NextHop";
            throw std::invalid_argument(err);
        }
        return ip_str;
    }

    std::string format_mpls() const
    {
        std::string str;
        if (!label_stack.empty())
        {
            if (outseg_type == SAI_OUTSEG_TYPE_SWAP)
            {
                str += "swap";
                str += LABELSTACK_DELIMITER;
            }
            else if (outseg_type == SAI_OUTSEG_TYPE_PUSH)
            {
                str += "push";
                str += LABELSTACK_DELIMITER;
            }
            str += label_stack.to_string() + LABELSTACK_DELIMITER;
        }
        return str;
    }
    
    NextHopKey ipKey() const
    {
        return NextHopKey(ip_address, alias);
    }
};

#endif /* SWSS_NEXTHOPKEY_H */

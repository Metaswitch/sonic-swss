#ifndef SWSS_NEXTHOPKEY_H
#define SWSS_NEXTHOPKEY_H

#include "label.h"
#include "ipaddress.h"
#include "tokenize.h"

#define LABELSTACK_DELIMITER '+'
#define NH_DELIMITER '@'
#define NHG_DELIMITER ','
#define VRF_PREFIX "Vrf"
extern IntfsOrch *gIntfsOrch;

struct NextHopKey
{
    LabelStack          label_stack;    // MPLS label stack
    IpAddress           ip_address;     // neighbor IP address
    string              alias;          // incoming interface alias
    uint8_t             weight;         // NH weight for NHGs

    NextHopKey() = default;
    NextHopKey(const std::string &ipstr, const std::string &alias, uint8_t weight = 1) : ip_address(ipstr), alias(alias), weight(weight) {}
    NextHopKey(const IpAddress &ip, const std::string &alias, uint8_t weight = 1) : ip_address(ip), alias(alias), weight(weight) {}
    NextHopKey(const std::string &str, uint8_t weight = 1) : weight(weight)
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
            SWSS_LOG_INFO("Labeled next hop");
            label_stack = LabelStack(str.substr(0, label_delimiter));
            ip_str = str.substr(label_delimiter+1);
        }
        else
        {
            SWSS_LOG_INFO("Unlabeled next hop");
            ip_str = str;
        }
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

    const std::string to_string() const
    {
        string str;
        if (!label_stack.empty())
        {
            str += label_stack.to_string();
            str += LABELSTACK_DELIMITER;
        }
        str += ip_address.to_string() + NH_DELIMITER + alias;
        return str;
    }

    bool operator<(const NextHopKey &o) const
    {
        return tie(label_stack, ip_address, alias) < tie(o.label_stack, o.ip_address, o.alias);
    }

    bool operator==(const NextHopKey &o) const
    {
        return (label_stack == o.label_stack) &&
            (ip_address == o.ip_address) && (alias == o.alias);
    }

    bool operator!=(const NextHopKey &o) const
    {
        return !(*this == o);
    }

    bool isIntfNextHop() const
    {
        return (ip_address.getV4Addr() == 0);
    }

    NextHopKey ipKey() const
    {
        return NextHopKey(ip_address, alias);
    }
};

#endif /* SWSS_NEXTHOPKEY_H */

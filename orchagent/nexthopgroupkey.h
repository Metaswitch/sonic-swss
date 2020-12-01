#ifndef SWSS_NEXTHOPGROUPKEY_H
#define SWSS_NEXTHOPGROUPKEY_H

#include "nexthopkey.h"
#include "assert.h"

class NextHopGroupKey
{
public:
    NextHopGroupKey() = default;

    /* ip_string|if_alias|vni|router_mac separated by ',' */
    NextHopGroupKey(const std::string &nexthops, bool overlay_nh, const std::string& weights = "")
    {
        m_overlay_nexthops = true;
        auto nhv = tokenize(nexthops, NHG_DELIMITER);
        auto wtv = tokenize(weights, NHG_DELIMITER);

        if (wtv.size() != nhv.size())
        {
            wtv.resize(nhv.size(), "1");
        }

        for (uint32_t i = 0; i < nhv.size(); i++)
        {
            m_nexthops.insert({NextHopKey(nhv[i], overlay_nh), std::stoi(wtv[i])});
        }
    }

    NextHopGroupKey(const std::string &nexthops, const std::string& weights = "")
    {
        std::vector<std::string> nhv = tokenize(nexthops, NHG_DELIMITER);
        std::vector<std::string> wtv = tokenize(weights, NHG_DELIMITER);

        if (wtv.size() != nhv.size())
        {
            wtv.resize(nhv.size(), "1");
        }

        for (uint32_t i = 0; i < nhv.size(); i++)
        {
            m_nexthops.insert({nhv[i], std::stoi(wtv[i])});
        }
    }

    inline std::set<NextHopKey> getNextHops() const
    {
        std::set<NextHopKey> nhs;
        for (const auto& it : m_nexthops)
        {
            nhs.insert(it.first);
        }
        return nhs;
    }

    inline const std::map<NextHopKey, uint8_t> &getNhsWithWts() const
    {
        return m_nexthops;
    }

    inline size_t getSize() const
    {
        return m_nexthops.size();
    }

    inline bool operator<(const NextHopGroupKey &o) const
    {
        return m_nexthops < o.m_nexthops;
    }

    inline bool operator==(const NextHopGroupKey &o) const
    {
        return m_nexthops == o.m_nexthops;
    }

    inline bool operator!=(const NextHopGroupKey &o) const
    {
        return !(*this == o);
    }

    void add(const std::string &ip,
            const std::string &alias,
            uint8_t weight = 1)
    {
        m_nexthops.insert({NextHopKey(ip, alias), weight});
    }

    void add(const std::string &nh, uint8_t weight = 1)
    {
        m_nexthops.insert({nh, weight});
    }

    void add(const NextHopKey &nh, uint8_t weight = 1)
    {
        m_nexthops.insert({nh, weight});
    }

    bool contains(const std::string &ip, const std::string &alias) const
    {
        NextHopKey nh(ip, alias);
        return m_nexthops.find(nh) != m_nexthops.end();
    }

    bool contains(const std::string &nh) const
    {
        return m_nexthops.find(nh) != m_nexthops.end();
    }

    bool contains(const NextHopKey &nh) const
    {
        return m_nexthops.find(nh) != m_nexthops.end();
    }

    bool contains(const NextHopGroupKey &nhs) const
    {
        for (const auto &it : nhs.getNextHops())
        {
            if (!contains(it))
            {
                return false;
            }
        }
        return true;
    }

    bool hasIntfNextHop() const
    {
        for (const auto &it : m_nexthops)
        {
            if (it.first.isIntfNextHop())
            {
                return true;
            }
        }
        return false;
    }

    void remove(const std::string &ip, const std::string &alias)
    {
        NextHopKey nh(ip, alias);
        m_nexthops.erase(nh);
    }

    void remove(const std::string &nh)
    {
        m_nexthops.erase(nh);
    }

    void remove(const NextHopKey &nh)
    {
        m_nexthops.erase(nh);
    }

    uint8_t getNextHopWeight(const NextHopKey& nh) const
    {
        return m_nexthops.at(nh);
    }

    const std::string to_string() const
    {
        string nhs_str;

        for (auto it = m_nexthops.begin(); it != m_nexthops.end(); ++it)
        {
            if (it != m_nexthops.begin())
            {
                nhs_str += NHG_DELIMITER;
            }
            if (m_overlay_nexthops) {
                nhs_str += it->first.to_string(m_overlay_nexthops);
            } else {
                nhs_str += it->first.to_string();
            }
        }

        return nhs_str;
    }

    inline bool is_overlay_nexthop() const
    {
        return m_overlay_nexthops;
    }

    void clear()
    {
        m_nexthops.clear();
    }

    std::map<NextHopKey, uint8_t> m_nexthops;
    bool m_overlay_nexthops;
};

#endif /* SWSS_NEXTHOPGROUPKEY_H */

#pragma once

#include "nhgorch.h"

class CbfNextHopGroup : public NextHopGroupBase
{
public:
    /*
     * Constructor.
     */
    CbfNextHopGroup(const std::string &index,
                    const std::unordered_set<std::string> &members,
                    const std::unordered_map<uint8_t, uint8_t> &class_map);
    CbfNextHopGroup(CbfNextHopGroup &&cbf_nhg);
    virtual ~CbfNextHopGroup();

    /*
     * Create the CBF group over SAI.
     */
    bool sync() override;

    /*
     * Remove the CBF group from SAI.
     */
    bool desync() override;

    /*
     * Update the CBF group, including the SAI programming.
     */
    bool update(const std::unordered_set<std::string> &members,
                const std::unordered_map<uint8_t, uint8_t> &class_map);

private:
    std::string m_key;
    std::unordered_set<std::string> m_members;
    std::unordered_map<uint8_t, uint8_t> m_class_map;

    /*
     * Build the SAI attribute for the class map.
     */
    sai_attribute_t getClassMapAttr() const;
};

struct CbfNhgEntry
{
    /*
     * Constructor.
     */
    explicit CbfNhgEntry(CbfNextHopGroup &&cbf_nhg);

    CbfNextHopGroup cbf_nhg;
    unsigned ref_count;
};

class CbfNhgOrch : public Orch
{
public:
    /*
     * Constructor.
     */
    CbfNhgOrch(DBConnector *db, const std::string &tableName);

    void doTask(Consumer &consumer) override;

private:
    std::unordered_map<std::string, CbfNhgEntry> m_syncdCbfNhgs;

    static std::tuple<bool,
                      std::unordered_set<std::string>,
                      std::unordered_map<uint8_t, uint8_t>>
                                    validateData(const std::string &members,
                                                 const std::string &class_map);
};

#pragma once

#include "nhgorch.h"

class CbfNhgMember : public NhgMember<std::string>
{
public:
    CbfNhgMember(const std::string &key) : NhgMember(key) {}

    /*
     * Sync the member, setting its SAI ID and incrementing the necessary
     * ref counters.
     */
    void sync(sai_object_id_t gm_id) override;

    /*
     * Desync the member, reseting its SAI ID and decrementing the necessary
     * ref counters.
     */
    void desync() override;

    /*
     * Get the NHG ID of this member.
     */
    sai_object_id_t getNhgId() const;

    /*
     * Set the index of this member.
     */
    bool setIndex(uint8_t index);

    /*
     * Get a string representation of this member.
     */
    std::string to_string() const override { return m_key; }

private:
    /*
     * The index of this member in the group's member list.
     */
    uint8_t m_index;
};

class CbfNextHopGroup : public NextHopGroupBase<std::string,
                                                std::string,
                                                CbfNhgMember>
{
public:
    /*
     * Constructors.
     */
    CbfNextHopGroup(const std::string &index,
                    const std::set<std::string> &members,
                    const std::unordered_map<uint8_t, uint8_t> &class_map);
    CbfNextHopGroup(CbfNextHopGroup &&cbf_nhg);

    /*
     * Destructor.
     */
    ~CbfNextHopGroup() { SWSS_LOG_ENTER(); desync(); }

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
    bool update(const std::set<std::string> &members,
                const std::unordered_map<uint8_t, uint8_t> &class_map);

private:
    std::unordered_map<uint8_t, uint8_t> m_class_map;

    /*
     * Sync the given members over SAI.
     */
    bool syncMembers(const std::set<std::string> &members) override;

    /*
     * Desync the given members from SAI.
     */
    bool desyncMembers(const std::set<std::string> &members) override;

    /*
     * Get the SAI attributes for creating the members over SAI.
     */
    std::vector<sai_attribute_t>
                    createNhgmAttrs(const CbfNhgMember &nhg) const override;

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
                      std::set<std::string>,
                      std::unordered_map<uint8_t, uint8_t>>
                                    validateData(const std::string &members,
                                                 const std::string &class_map);
};

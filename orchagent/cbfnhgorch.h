#pragma once

#include "nexthopgroup.h"

using namespace std;

class CbfNhgMember : public NhgMember<string>
{
public:
    CbfNhgMember(const string &key, uint8_t index) :
        NhgMember(key), m_index(index) { SWSS_LOG_ENTER(); }

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
     * Get the index of this group member.
     */
    uint8_t getIndex() const { return m_index; }

    /*
     * Set the index of this member.
     */
    bool setIndex(uint8_t index);

    /*
     * Get a string representation of this member.
     */
    string to_string() const override { return m_key; }

private:
    /*
     * The index of this member in the group's member list.
     */
    uint8_t m_index;
};

class CbfNextHopGroup : public NhgCommon<string, string, CbfNhgMember>
{
public:
    /*
     * Constructors.
     */
    CbfNextHopGroup(const string &index,
                    const vector<string> &members,
                    const unordered_map<uint8_t, uint8_t> &class_map);
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
     * CBF groups can never be temporary.
     */
    inline bool isTemp() const override { SWSS_LOG_ENTER(); return false; }

    /*
     * CBF groups do not have a NextHopGroupkey.
     */
    inline NextHopGroupKey getNhgKey() const override { return {}; }

    /*
     * Update the CBF group, including the SAI programming.
     */
    bool update(const vector<string> &members,
                const unordered_map<uint8_t, uint8_t> &class_map);

    string to_string() const override { return m_key; }

private:
    unordered_map<uint8_t, uint8_t> m_class_map;

    /*
     * Sync the given members over SAI.
     */
    bool syncMembers(const set<string> &members) override;

    /*
     * Desync the given members from SAI.
     */
    bool desyncMembers(const set<string> &members) override;

    /*
     * Get the SAI attributes for creating the members over SAI.
     */
    vector<sai_attribute_t>
                    createNhgmAttrs(const CbfNhgMember &nhg) const override;

    /*
     * Build the SAI attribute for the class map.
     */
    sai_attribute_t getClassMapAttr() const;
};

class CbfNhgOrch : public NhgOrchCommon<CbfNextHopGroup>
{
public:
    void doTask(Consumer &consumer);

private:
    static tuple<bool, vector<string>, unordered_map<uint8_t, uint8_t>>
                validateData(const string &members, const string &class_map);
};

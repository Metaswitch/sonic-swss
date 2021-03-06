#pragma once

#include "nexthopgroup.h"
#include "nhghandler.h"

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
     * Update the NEXT_HOP attribute of this member.
     */
    bool updateNhAttr();

    /*
     * Get the index of this group member.
     */
    uint8_t getIndex() const { return m_index; }

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

class CbfNhg : public NhgCommon<string, string, CbfNhgMember>
{
public:
    /*
     * Constructors.
     */
    CbfNhg(const string &index,
            const vector<string> &members,
            const unordered_map<uint8_t, uint8_t> &class_map);
    CbfNhg(CbfNhg &&cbf_nhg);

    /*
     * Destructor.
     */
    ~CbfNhg() { SWSS_LOG_ENTER(); desync(); }

    /*
     * Create the CBF group over SAI.
     */
    bool sync() override;

    /*
     * CBF groups can never be temporary.
     */
    inline bool isTemp() const override { SWSS_LOG_ENTER(); return false; }

    /*
     * Check if the CBF next hop group contains synced temporary NHGs.
     */
    inline bool hasTemps() const
                            { SWSS_LOG_ENTER(); return !m_temp_nhgs.empty(); }

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
     * Map of synced temporary NHGs contained in this next hop group along with
     * the NHG ID at the time of sync.
     */
    unordered_map<string, sai_object_id_t> m_temp_nhgs;

    /*
     * Sync the given members over SAI.
     */
    bool syncMembers(const set<string> &members) override;

    /*
     * Get the SAI attributes for creating the members over SAI.
     */
    vector<sai_attribute_t>
                    createNhgmAttrs(const CbfNhgMember &nhg) const override;

    /*
     * Build the SAI attribute for the class map.
     */
    sai_attribute_t getClassMapAttr() const;

    /*
     * Check if the CBF NHG has the same members and in the same order as the
     * ones given.
     */
    bool hasSameMembers(const vector<string> &members) const;
};

class CbfNhgHandler : public NhgHandlerCommon<CbfNhg>
{
public:
    void doTask(Consumer &consumer);

    /*
     * Get the non CBF NHG with the given index.
     */
    static inline const Nhg& getNonCbfNhg(const string &index);

private:
    static tuple<bool, vector<string>, unordered_map<uint8_t, uint8_t>>
                validateData(const string &members, const string &class_map);
};

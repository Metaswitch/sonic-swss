#pragma once

#include "nexthopgroup.h"

using namespace std;

class WeightedNhgMember : public NhgMember<NextHopKey>
{
public:
    WeightedNhgMember(const pair<NextHopKey, uint8_t>& nhgm) :
        NhgMember(nhgm.first), m_weight(nhgm.second) {}

    /* Constructors / Assignment operators. */
    WeightedNhgMember(const NextHopKey& nh_key, uint8_t weight) :
        NhgMember(nh_key), m_weight(weight) {}

    WeightedNhgMember(WeightedNhgMember&& nhgm) :
        NhgMember(move(nhgm)),
        m_weight(nhgm.m_weight) {}

    WeightedNhgMember& operator=(WeightedNhgMember&& nhgm);

    /* Destructor. */
    ~WeightedNhgMember();

    /* Update member's weight and update the SAI attribute as well. */
    bool updateWeight(uint8_t weight);

    /* Sync / Desync. */
    void sync(sai_object_id_t gm_id) override;
    void desync() override;

    /* Getters / Setters. */
    inline uint8_t getWeight() const { return m_weight; }
    sai_object_id_t getNhId() const;

    /* Check if the next hop is labeled. */
    inline bool isLabeled() const { return !m_key.label_stack.empty(); }

    /* Convert member's details to string. */
    string to_string() const override
    {
        return m_key.to_string() +
                ", weight: " + std::to_string(m_weight) +
                ", SAI ID: " + std::to_string(m_id);
    }

private:
    /* Weight of the next hop. */
    uint8_t m_weight;
};

/*
 * NextHopGroup class representing a next hop group object.
 */
class NextHopGroup : public NhgCommon<NextHopGroupKey,
                                        NextHopKey,
                                        WeightedNhgMember>
{
public:
    /* Constructors. */
    explicit NextHopGroup(const NextHopGroupKey& key);

    NextHopGroup(NextHopGroup&& nhg) :
        NhgCommon(move(nhg)), m_is_temp(nhg.m_is_temp)
    { SWSS_LOG_ENTER(); }

    NextHopGroup& operator=(NextHopGroup&& nhg);

    ~NextHopGroup() { SWSS_LOG_ENTER(); desync(); }

    /* Sync the group, creating the group's and members SAI IDs. */
    bool sync() override;

    /* Desync the group, reseting the group's and members SAI IDs.  */
    bool desync() override;

    /*
     * Update the group based on a new next hop group key.  This will also
     * perform any sync / desync necessary.
     */
    bool update(const NextHopGroupKey& nhg_key);

    /* Validate a next hop in the group, syncing it. */
    bool validateNextHop(const NextHopKey& nh_key);

    /* Invalidate a next hop in the group, desyncing it. */
    bool invalidateNextHop(const NextHopKey& nh_key);

    /* Getters / Setters. */
    inline bool isTemp() const override { return m_is_temp; }
    inline void setTemp(bool is_temp) { m_is_temp = is_temp; }

    NextHopGroupKey getNhgKey() const override { return m_key; }

    /* Convert NHG's details to a string. */
    string to_string() const override
    {
        return m_key.to_string() + ", SAI ID: " + std::to_string(m_id);
    }

private:
    /* Whether the group is temporary or not. */
    bool m_is_temp;

    /* Add group's members over the SAI API for the given keys. */
    bool syncMembers(const set<NextHopKey>& nh_keys) override;

    /* Remove group's members the SAI API from the given keys. */
    bool desyncMembers(const set<NextHopKey>& nh_keys) override;

    /* Create the attributes vector for a next hop group member. */
    vector<sai_attribute_t> createNhgmAttrs(
                                const WeightedNhgMember& nhgm) const override;
};

/*
 * Next Hop Group Orchestrator class that handles NEXT_HOP_GROUP_TABLE
 * updates.
 */
class NonCbfNhgOrch : public NhgOrchCommon<NextHopGroup>
{
public:
    /*
     * Constructor.
     */
    NonCbfNhgOrch(DBConnector *db, const string &tableName) :
        NhgOrchCommon(db, tableName) { SWSS_LOG_ENTER(); }

    /* Add a temporary next hop group when resources are exhausted. */
    NextHopGroup createTempNhg(const NextHopGroupKey& nhg_key);

    /* Validate / Invalidate a next hop. */
    bool validateNextHop(const NextHopKey& nh_key);
    bool invalidateNextHop(const NextHopKey& nh_key);

    void doTask(Consumer &consumer) override;
};

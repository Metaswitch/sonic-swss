#pragma once

#include "orch.h"
#include "nexthopgroupkey.h"
#include "crmorch.h"

extern CrmOrch *gCrmOrch;

template <typename Index>
class NhgMember
{
public:
    /*
     * Constructors.
     */
    explicit NhgMember(const Index &key) : m_key(key), m_id(SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ENTER();
    }

    NhgMember(NhgMember &&nhgm) : m_key(std::move(nhgm.m_key)), m_id(nhgm.m_id)
    {
        SWSS_LOG_ENTER();
        nhgm.m_id = SAI_NULL_OBJECT_ID;
    }

    NhgMember& operator=(NhgMember &&nhgm)
    {
        SWSS_LOG_ENTER();

        std::swap(m_key, nhgm.m_key);
        std::swap(m_id, nhgm.m_id);

        return *this;
    }

    /*
     * Prevent copying.
     */
    NhgMember(const NhgMember&) = delete;
    void operator=(const NhgMember&) = delete;

    /*
     * Destructor.
     */
    virtual ~NhgMember()
    {
        SWSS_LOG_ENTER();
        assert(!isSynced());
    }

    /*
     * Sync the NHG member, setting its SAI ID.
     */
    virtual void sync(sai_object_id_t gm_id)
    {
        SWSS_LOG_ENTER();

        SWSS_LOG_INFO("Syncing next hop group member %s", to_string().c_str());

        /*
         * The SAI ID should be updated from invalid to something valid.
         */
        if ((m_id != SAI_NULL_OBJECT_ID) || (gm_id == SAI_NULL_OBJECT_ID))
        {
            SWSS_LOG_ERROR("Setting invalid SAI ID %lu to next hop group "
                            "member %s, with current SAI ID %lu",
                            gm_id, to_string().c_str(), m_id);
            throw std::logic_error("Invalid SAI ID assigned to next hop group"
                                    " member");
        }

        m_id = gm_id;
        gCrmOrch->incCrmResUsedCounter(
                                    CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
    }

    /*
     * Desync the group member, resetting its SAI ID.
     */
    virtual void desync()
    {
        SWSS_LOG_ENTER();

        /*
         * The group member should be synced.
         */
        if (!isSynced())
        {
            SWSS_LOG_ERROR("Desyincing next hop group member %s which is "
                            "already desynced", to_string().c_str());
            throw std::logic_error("Desyncing already desynced next hop group"
                                    " member");
        }

        m_id = SAI_NULL_OBJECT_ID;
        gCrmOrch->decCrmResUsedCounter(
                                    CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
    }

    /*
     * Getters.
     */
    inline Index getKey() const { return m_key; }
    inline sai_object_id_t getId() const { return m_id; }

    /*
     * Check whether the group is synced.
     */
    inline bool isSynced() const { return m_id != SAI_NULL_OBJECT_ID; }

    /*
     * Get a string form of the member.
     */
    virtual std::string to_string() const = 0;

protected:
    /*
     * The index / key of this NHG member.
     */
    Index m_key;

    /*
     * The SAI ID of this NHG member.
     */
    sai_object_id_t m_id;
};

class WeightedNhgMember : public NhgMember<NextHopKey>
{
public:
    WeightedNhgMember(const std::pair<NextHopKey, uint8_t>& nhgm) :
        NhgMember(nhgm.first), m_weight(nhgm.second) {}

    /* Constructors / Assignment operators. */
    WeightedNhgMember(const NextHopKey& nh_key, uint8_t weight) :
        NhgMember(nh_key), m_weight(weight) {}

    WeightedNhgMember(WeightedNhgMember&& nhgm) :
        NhgMember(std::move(nhgm)),
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
    std::string to_string() const override
    {
        return m_key.to_string() +
                ", weight: " + std::to_string(m_weight) +
                ", SAI ID: " + std::to_string(m_id);
    }

private:
    /* Weight of the next hop. */
    uint8_t m_weight;
};

/* Map indexed by NextHopKey, containing the SAI ID of the group member. */
typedef std::map<NextHopKey, WeightedNhgMember> NhgMembers;

/*
 * NextHopGroupBase class representing the common base class between
 * NextHopGroup and CbfNextHopGroup classes.
 */
template <typename Key, typename MbrIndex, typename Mbr>
class NextHopGroupBase
{
public:
    /*
     * Constructors.
     */
    explicit NextHopGroupBase(const Key &key) :
        m_key(key),
        m_id(SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ENTER();
    }

    NextHopGroupBase(NextHopGroupBase &&nhg) :
        m_key(std::move(nhg.m_key)),
        m_id(nhg.m_id),
        m_members(std::move(nhg.m_members))
    {
        SWSS_LOG_ENTER();

        /*
         * Invalidate the rvalue SAI ID.
         */
        nhg.m_id = SAI_NULL_OBJECT_ID;
    }

    NextHopGroupBase& operator=(NextHopGroupBase &&nhg)
    {
        SWSS_LOG_ENTER();

        std::swap(m_key, nhg.m_key);
        std::swap(m_id, nhg.m_id);
        std::swap(m_members, nhg.m_members);

        return *this;
    }

    virtual ~NextHopGroupBase() = default;

    /*
     * Prevent copying.
     */
    NextHopGroupBase(const NextHopGroupBase&) = delete;
    NextHopGroupBase& operator=(const NextHopGroupBase&) = delete;

    /*
     * Check if the next hop group is synced or not.
     */
    inline bool isSynced() const { return m_id != SAI_NULL_OBJECT_ID; }

    /*
     * Sync the group, generating a SAI ID.
     */
    virtual bool sync() = 0;

    /*
     * Desync the group, releasing the SAI ID.
     */
    virtual bool desync() = 0;

    /*
     * Check if the group contains the given member.
     */
    inline bool hasMember(const MbrIndex &key) const
                            { return m_members.find(key) != m_members.end(); }

    /*
     * Getters.
     */
    inline Key getKey() const { return m_key; }
    inline sai_object_id_t getId() const { return m_id; }
    inline size_t getSize() const { return m_members.size(); }

protected:
    /*
     * The key indexing this object.
     */
    Key m_key;

    /*
     * The SAI ID of this object.
     */
    sai_object_id_t m_id;

    /*
     * The members of this group.
     */
    std::map<MbrIndex, Mbr> m_members;

    /*
     * Sync the given members in the group.
     */
    virtual bool syncMembers(const std::set<MbrIndex> &mbr_indexes) = 0;

    /*
     * Desync the given members from the group.
     */
    virtual bool desyncMembers(const std::set<MbrIndex> &mbr_indexes) = 0;

    /*
     * Get the SAI attributes for creating a next hop group member over SAI.
     */
    virtual std::vector<sai_attribute_t> createNhgmAttrs(const Mbr &member)
                                                                    const = 0;
};

/*
 * NextHopGroup class representing a next hop group object.
 */
class NextHopGroup : public NextHopGroupBase<NextHopGroupKey,
                                            NextHopKey,
                                            WeightedNhgMember>
{
public:
    /* Constructors. */
    explicit NextHopGroup(const NextHopGroupKey& key);
    NextHopGroup(NextHopGroup&& nhg) :
                NextHopGroupBase(std::move(nhg)), m_is_temp(nhg.m_is_temp) {}
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
    inline bool isTemp() const { return m_is_temp; }
    inline void setTemp(bool is_temp) { m_is_temp = is_temp; }

    /* Convert NHG's details to a string. */
    std::string to_string() const
    {
        return m_key.to_string() + ", SAI ID: " + std::to_string(m_id);
    }

    /* Increment the number of existing groups. */
    static inline void incCount() { ++m_count; }

    /* Decrement the number of existing groups. */
    static inline void decCount() { assert(m_count > 0); --m_count; }

    static inline unsigned getCount() { return m_count; }

private:
    /* Whether the group is temporary or not. */
    bool m_is_temp;

    /* Add group's members over the SAI API for the given keys. */
    bool syncMembers(const std::set<NextHopKey>& nh_keys) override;

    /* Remove group's members the SAI API from the given keys. */
    bool desyncMembers(const std::set<NextHopKey>& nh_keys) override;

    /* Create the attributes vector for a next hop group member. */
    vector<sai_attribute_t> createNhgmAttrs(
                                const WeightedNhgMember& nhgm) const override;

    /*
     * Number of existing NHGs.  Incremented when an object is created and
     * decremented when an object is destroyed.  This will also account for the
     * groups created by RouteOrch.
     */
    static unsigned m_count;
};

/*
 * Structure describing a next hop group which NhgOrch owns.  Beside having a
 * unique pointer to that next hop group, we also want to keep a ref count so
 * NhgOrch knows how many other objects reference the next hop group in order
 * not to delete them while still being referenced.
 */
struct NhgEntry
{
    /* Pointer to the next hop group.  NhgOrch is the sole owner of it. */
    std::unique_ptr<NextHopGroup> nhg;

    /* Number of external objects referencing this next hop group. */
    unsigned int ref_count;

    NhgEntry() = default;
    explicit NhgEntry(std::unique_ptr<NextHopGroup>&& _nhg,
                      unsigned int _ref_count = 0) :
        nhg(std::move(_nhg)), ref_count(_ref_count) {}
};

/*
 * Map indexed by next hop group's CP ID, containing the next hop group for
 * that ID and the number of objects referencing it.
 */
typedef std::unordered_map<std::string, NhgEntry> NhgTable;

/*
 * Next Hop Group Orchestrator class that handles NEXT_HOP_GROUP_TABLE
 * updates.
 */
class NhgOrch : public Orch
{
public:
    /*
     * Constructor.
     */
    NhgOrch(DBConnector *db, const std::string &tableName);

    /* Check if the next hop group given by it's index exists. */
    inline bool hasNhg(const std::string& index) const
        { return m_syncdNextHopGroups.find(index) !=
                                                m_syncdNextHopGroups.end(); }

    /*
     * Get the next hop group given by it's index.  If the index does not exist
     * in map, a std::out_of_range exception will be thrown.
     */
    inline const NextHopGroup& getNhg(const std::string& index) const
                        { return *m_syncdNextHopGroups.at(index).nhg; }

    /* Add a temporary next hop group when resources are exhausted. */
    NextHopGroup createTempNhg(const NextHopGroupKey& nhg_key);

    /* Getters / Setters. */
    inline unsigned int getMaxNhgCount() const { return m_maxNhgCount; }
    static inline unsigned int getNhgCount()
                                        { return NextHopGroup::getCount(); }

    /* Validate / Invalidate a next hop. */
    bool validateNextHop(const NextHopKey& nh_key);
    bool invalidateNextHop(const NextHopKey& nh_key);

    /* Increase / Decrease the number of next hop groups. */
    inline void incNhgCount()
    {
        assert(NextHopGroup::getCount() < m_maxNhgCount);
        NextHopGroup::incCount();
    }
    inline void decNhgCount() { NextHopGroup::decCount(); }

    /* Increase / Decrease ref count for a NHG given by it's index. */
    void incNhgRefCount(const std::string& index);
    void decNhgRefCount(const std::string& index);

    void doTask(Consumer &consumer) override;

private:

    /*
     * Switch's maximum number of next hop groups capacity.
     */
    unsigned int m_maxNhgCount;

    /*
     * The next hop group table.
     */
    NhgTable m_syncdNextHopGroups;
};

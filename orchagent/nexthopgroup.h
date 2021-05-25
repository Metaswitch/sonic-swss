#pragma once

#include "string"
#include "logger.h"
#include "saitypes.h"
#include "unordered_map"
#include "dbconnector.h"
#include "set"
#include "orch.h"
#include "crmorch.h"
#include "nexthopgroupkey.h"

using namespace std;

extern CrmOrch *gCrmOrch;

template <typename Key>
class NhgMember
{
public:
    explicit NhgMember(const Key &key) :
        m_key(key), m_id(SAI_NULL_OBJECT_ID) { SWSS_LOG_ENTER(); }

    NhgMember(NhgMember &&nhgm) : m_key(move(nhgm.m_key)), m_id(nhgm.m_id)
        { SWSS_LOG_ENTER(); nhgm.m_id = SAI_NULL_OBJECT_ID; }

    NhgMember& operator=(NhgMember &&nhgm)
    {
        SWSS_LOG_ENTER();

        swap(m_key, nhgm.m_key);
        swap(m_id, nhgm.m_id);

        return *this;
    }

    /*
     * Prevent copying.
     */
    NhgMember(const NhgMember&) = delete;
    void operator=(const NhgMember&) = delete;

    virtual ~NhgMember()
    {
        SWSS_LOG_ENTER();

        if (isSynced())
        {
            SWSS_LOG_ERROR("Deleting next hop group member which is still "
                            "synced");
            assert(false);
        }
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
                            "membeer %s, with current SAI ID %lu",
                            gm_id, to_string().c_str(), m_id);
            throw logic_error("Invalid SAI ID assigned to next hop group "
                                "member");
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
        * If the membeer is not synced, exit.
        */
        if (!isSynced())
        {
            return;
        }

        m_id = SAI_NULL_OBJECT_ID;
        gCrmOrch->decCrmResUsedCounter(
                                    CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
    }

    /*
     * Getters.
     */
    inline Key getKey() const { return m_key; }
    inline sai_object_id_t getId() const { return m_id; }

    /*
     * Check whether the group is synced.
     */
    inline bool isSynced() const { return m_id != SAI_NULL_OBJECT_ID; }

    /*
     * Get a string form of the member.
     */
    virtual string to_string() const = 0;

protected:
    /*
     * The index / key of this NHG member.
     */
    Key m_key;

    /*
     * The SAI ID of this NHG member.
     */
    sai_object_id_t m_id;
};

/*
 * Base class for next hop groups, containing the common interface that every
 * next hop group should have based on what RouteOrch needs when working with
 * next hop groups.
 */
class NhgBase
{
public:
    NhgBase() : m_id(SAI_NULL_OBJECT_ID) { SWSS_LOG_ENTER(); }

    NhgBase(NhgBase &&nhg) : m_id(nhg.m_id)
        { SWSS_LOG_ENTER(); nhg.m_id = SAI_NULL_OBJECT_ID; }

    NhgBase& operator=(NhgBase &&nhg)
        { SWSS_LOG_ENTER(); swap(m_id, nhg.m_id); return *this; }

    /*
     * Prevent copying.
     */
    NhgBase(const NhgBase&) = delete;
    void operator=(const NhgBase&) = delete;

    virtual ~NhgBase();

    /*
     * Getters.
     */
    inline sai_object_id_t getId() const { SWSS_LOG_ENTER(); return m_id; }
    static inline unsigned getSyncedCount()
                                    { SWSS_LOG_ENTER(); return m_syncedCount; }

    /*
     * Sync the group, generating a SAI ID.
     */
    virtual bool sync() = 0;

    /*
     * Desync the group, releasing the SAI ID.
     */
    virtual bool desync() = 0;

    /*
     * Check if the next hop group is synced or not.
     */
    inline bool isSynced() const { return m_id != SAI_NULL_OBJECT_ID; }

    /*
     * Check if the next hop group is temporary.
     */
    virtual bool isTemp() const = 0;

    /*
     * Get the NextHopGroupKey of this object.
     */
    virtual NextHopGroupKey getNhgKey() const = 0;

    /* Increment the number of existing groups. */
    static inline void incSyncedCount() { SWSS_LOG_ENTER(); ++m_syncedCount; }

    /* Decrement the number of existing groups. */
    static void decSyncedCount();

protected:
    /*
     * The SAI ID of this object.
     */
    sai_object_id_t m_id;

    /*
     * Number of synced NHGs.  Incremented when an object is synced and
     * decremented when an object is desynced.  This will also account for the
     * groups created by RouteOrch.
     */
    static unsigned m_syncedCount;
};

/*
 * NhgCommon class representing the common base class between
 * NextHopGroup and CbfNextHopGroup classes.
 */
template <typename Key, typename MbrKey, typename Mbr>
class NhgCommon : public NhgBase
{
public:
    /*
     * Constructors.
     */
    explicit NhgCommon(const Key &key) : m_key(key) { SWSS_LOG_ENTER(); }

    NhgCommon(NhgCommon &&nhg) : NhgBase(move(nhg)),
                                m_key(move(nhg.m_key)),
                                m_members(move(nhg.m_members))
    { SWSS_LOG_ENTER(); }

    NhgCommon& operator=(NhgCommon &&nhg)
    {
        SWSS_LOG_ENTER();

        swap(m_key, nhg.m_key);
        swap(m_members, nhg.m_members);

        NhgBase::operator=(move(nhg));

        return *this;
    }

    /*
     * Check if the group contains the given member.
     */
    inline bool hasMember(const MbrKey &key) const
        { SWSS_LOG_ENTER(); return m_members.find(key) != m_members.end(); }

    /*
     * Getters.
     */
    inline Key getKey() const { SWSS_LOG_ENTER(); return m_key; }
    inline size_t getSize() const
                                { SWSS_LOG_ENTER(); return m_members.size(); }

    /*
     * Get a string representation of this next hop group.
     */
    virtual string to_string() const = 0;

protected:
    /*
     * The key indexing this object.
     */
    Key m_key;

    /*
     * The members of this group.
     */
    map<MbrKey, Mbr> m_members;

    /*
     * Sync the given members in the group.
     */
    virtual bool syncMembers(const set<MbrKey> &mbr_indexes) = 0;

    /*
     * Desync the given members from the group.
     */
    virtual bool desyncMembers(const set<MbrKey> &mbr_indexes) = 0;

    /*
     * Get the SAI attributes for creating a next hop group member over SAI.
     */
    virtual vector<sai_attribute_t> createNhgmAttrs(const Mbr &member)
                                                                    const = 0;
};

/*
 * Structure describing a next hop group which NhgOrch owns.  Beside having a
 * next hop group, we also want to keep a ref count so we don't delete objects
 * that are still referenced.
 */
template <typename NhgClass>
struct NhgEntry
{
    /* The next hop group object in this entry. */
    NhgClass nhg;

    /* Number of external objects referencing this next hop group. */
    unsigned ref_count;

    NhgEntry() = default;
    explicit NhgEntry(NhgClass&& _nhg, unsigned int _ref_count = 0) :
                nhg(move(_nhg)), ref_count(_ref_count) { SWSS_LOG_ENTER(); }
};

/*
 * Class providing the common functionality shared by all NhgOrch classes.
 */
template <typename NhgClass>
class NhgOrchCommon
{
public:
    /*
     * Check if the given next hop group index exists.
     */
    inline bool hasNhg(const string &index) const
    {
        SWSS_LOG_ENTER();
        return m_syncedNhgs.find(index) != m_syncedNhgs.end();
    }

    /*
     * Get the next hop group with the given index.  If the index does not
     * exist in the map, a out_of_range eexception will be thrown.
     */
    inline const NhgClass& getNhg(const string &index) const
                    { SWSS_LOG_ENTER(); return m_syncedNhgs.at(index).nhg; }

    /* Increase the ref count for a NHG given by it's index. */
    void incNhgRefCount(const string& index)
    {
        SWSS_LOG_ENTER();

        auto& nhg_entry = m_syncedNhgs.at(index);

        SWSS_LOG_INFO("Increment group %s ref count from %u to %u",
                        index.c_str(),
                        nhg_entry.ref_count,
                        nhg_entry.ref_count + 1);

        ++nhg_entry.ref_count;
    }

    /* Dencrease the ref count for a NHG given by it's index. */
    void decNhgRefCount(const string& index)
    {
        SWSS_LOG_ENTER();

        auto& nhg_entry = m_syncedNhgs.at(index);

        /* Sanity check so we don't overflow. */
        if (nhg_entry.ref_count == 0)
        {
            SWSS_LOG_ERROR("Trying to decrement next hop group %s reference "
                            "count while none are left.",
                            nhg_entry.nhg.to_string().c_str());
            throw logic_error("Decreasing ref count which is already 0");
        }

        SWSS_LOG_INFO("Decrement group %s ref count from %u to %u",
                        index.c_str(),
                        nhg_entry.ref_count,
                        nhg_entry.ref_count - 1);

        --nhg_entry.ref_count;
    }

protected:
    /*
     * Map of synced next hop groups.
     */
    unordered_map<string, NhgEntry<NhgClass>> m_syncedNhgs;
};

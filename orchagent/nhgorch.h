#pragma once

#include "cbfnhgorch.h"
#include "noncbfnhgorch.h"
#include "switchorch.h"

/* Default maximum number of next hop groups */
#define DEFAULT_NUMBER_OF_ECMP_GROUPS   128
#define DEFAULT_MAX_ECMP_GROUP_SIZE     32

extern sai_object_id_t gSwitchId;

extern SwitchOrch *gSwitchOrch;

extern sai_switch_api_t *sai_switch_api;

class NhgOrch
{
public:
    NhgOrch(DBConnector *db) :
        nonCbfNhgOrch(db, APP_NEXT_HOP_GROUP_TABLE_NAME),
        cbfNhgOrch(db, APP_CLASS_BASED_NEXT_HOP_GROUP_TABLE_NAME)
    {
        SWSS_LOG_ENTER();

        /* Get the switch's maximum next hop group capacity. */
        sai_attribute_t attr;
        attr.id = SAI_SWITCH_ATTR_NUMBER_OF_ECMP_GROUPS;

        sai_status_t status = sai_switch_api->get_switch_attribute(gSwitchId,
                                                                1,
                                                                &attr);

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_WARN("Failed to get switch attribute number of ECMP "
                            "groups. Use default value. rv:%d", status);
            m_maxNhgCount = DEFAULT_NUMBER_OF_ECMP_GROUPS;
        }
        else
        {
            m_maxNhgCount = attr.value.s32;

            /*
            * ASIC specific workaround to re-calculate maximum ECMP groups
            * according to diferent ECMP mode used.
            *
            * On Mellanox platform, the maximum ECMP groups returned is the value
            * under the condition that the ECMP group size is 1. Deviding this
            * number by DEFAULT_MAX_ECMP_GROUP_SIZE gets the maximum number of
            * ECMP groups when the maximum ECMP group size is 32.
            */
            char *platform = getenv("platform");
            if (platform && strstr(platform, MLNX_PLATFORM_SUBSTRING))
            {
                SWSS_LOG_NOTICE("Mellanox platform - divide capacity by %d",
                                DEFAULT_MAX_ECMP_GROUP_SIZE);
                m_maxNhgCount /= DEFAULT_MAX_ECMP_GROUP_SIZE;
            }
        }

        /* Set switch's next hop group capacity. */
        std::vector<swss::FieldValueTuple> fvTuple;
        fvTuple.emplace_back("MAX_NEXTHOP_GROUP_COUNT",
                                std::to_string(m_maxNhgCount));
        gSwitchOrch->set_switch_capability(fvTuple);

        SWSS_LOG_NOTICE("Maximum number of ECMP groups supported is %d",
                        m_maxNhgCount);
    }

    /*
     * Get the maximum number of ECMP groups allowed by the switch.
     */
    static inline unsigned getMaxNhgCount()
                                    { SWSS_LOG_ENTER(); return m_maxNhgCount; }

    /*
     * Get the number of next hop groups that are synced.
     */
    static inline unsigned getSyncedNhgCount()
                        { SWSS_LOG_ENTER(); return NhgBase::getSyncedCount(); }

    /* Increase the number of synced next hop groups. */
    static void incSyncedNhgCount()
    {
        SWSS_LOG_ENTER();

        if (getSyncedNhgCount() >= m_maxNhgCount)
        {
            SWSS_LOG_ERROR("Incresing synced next hop group count beyond "
                            "switch's capabilities");
            throw logic_error("Next hop groups exceed switch's "
                                    "capabilities");
        }

        NhgBase::incSyncedCount();
    }

    /* Decrease the number of next hop groups. */
    static inline void decSyncedNhgCount()
                            { SWSS_LOG_ENTER(); NhgBase::decSyncedCount(); }

    /*
     * Check if the next hop group with the given index exists.
     */
    inline bool hasNhg(const string &index) const
    {
        SWSS_LOG_ENTER();
        return nonCbfNhgOrch.hasNhg(index) || cbfNhgOrch.hasNhg(index);
    }

    /*
     * Get the next hop group with the given index.
     */
    const NhgBase& getNhg(const string &index) const
    {
        SWSS_LOG_ENTER();

        try
        {
            return nonCbfNhgOrch.getNhg(index);
        }
        catch(const std::out_of_range &e)
        {
            return cbfNhgOrch.getNhg(index);
        }
    }

    /*
     * Increase the reference counter for the next hop group with the given
     * index.
     */
    void incNhgRefCount(const string &index)
    {
        SWSS_LOG_ENTER();

        if (nonCbfNhgOrch.hasNhg(index))
        {
            nonCbfNhgOrch.incNhgRefCount(index);
        }
        else if (cbfNhgOrch.hasNhg(index))
        {
            cbfNhgOrch.incNhgRefCount(index);
        }
        else
        {
            throw std::out_of_range("Next hop group index not found.");
        }
    }

    /*
     * Decrease the reference counter for the next hop group with the given
     * index.
     */
    void decNhgRefCount(const string &index)
    {
        SWSS_LOG_ENTER();

        if (nonCbfNhgOrch.hasNhg(index))
        {
            nonCbfNhgOrch.decNhgRefCount(index);
        }
        else if (cbfNhgOrch.hasNhg(index))
        {
            cbfNhgOrch.decNhgRefCount(index);
        }
        else
        {
            throw std::out_of_range("Next hop group index not found.");
        }
    }

    NonCbfNhgOrch nonCbfNhgOrch;
    CbfNhgOrch cbfNhgOrch;

private:
    /*
     * Switch's maximum number of next hop groups capacity.
     */
    static unsigned m_maxNhgCount;
};

unsigned NhgOrch::m_maxNhgCount = 0;

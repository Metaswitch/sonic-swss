#include "nexthopgroup.h"
#include "switchorch.h"
#include "vector"
#include "rediscommand.h"

extern sai_object_id_t gSwitchId;
extern SwitchOrch *gSwitchOrch;

extern sai_switch_api_t *sai_switch_api;

unsigned NhgOrchBase::m_maxNhgCount = 0;
unsigned NhgBase::m_count = 0;


/* Default maximum number of next hop groups */
#define DEFAULT_NUMBER_OF_ECMP_GROUPS   128
#define DEFAULT_MAX_ECMP_GROUP_SIZE     32

/*
 * Purpose: Destructor.
 *
 * Params:  None.
 *
 * Returns: Nothing.
 */
NhgBase::~NhgBase()
{
    SWSS_LOG_ENTER();

    if (isSynced())
    {
        SWSS_LOG_ERROR("Destroying next hop group with SAI ID %lu which is"
                        " still synced.", m_id);
        assert(false);
    }
}

/*
 * Purpose: Decrease the count of synced next hop group objects.
 *
 * Params:  None.
 *
 * Returns: Nothing.
 */
void NhgBase::decCount()
{
    SWSS_LOG_ENTER();

    if (m_count == 0)
    {
        SWSS_LOG_ERROR("Decreasing next hop groups count while already 0");
        throw logic_error("Decreasing next hop groups count while "
                                "already 0");
    }

    --m_count;
}

/*
 * Purpose: Set the switch ECMP limit.
 *
 * Params:  None.
 *
 * Returns: Nothing.
 */
void NhgOrchBase::setSwtichEcmpLimit()
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
 * Purpose: Increase the number of synced next hop groups.
 *
 * Params:  None.
 *
 * Returns: Nothing.
 */
void NhgOrchBase::incNhgCount()
{
    SWSS_LOG_ENTER();

    if (NhgBase::getCount() >= m_maxNhgCount)
    {
        SWSS_LOG_ERROR("Incresing synced next hop group count beyond "
                        "switch's capabilities");
        throw logic_error("Next hop groups exceed switch's "
                                "capabilities");
    }

    NhgBase::incCount();
}

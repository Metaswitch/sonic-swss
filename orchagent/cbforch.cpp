#include "cbforch.h"

extern sai_qos_map_api_t *sai_qos_map_api;
extern sai_object_id_t gSwitchId;

CbfOrch::CbfOrch(DBConnector *db,
                 const vector<table_name_with_pri_t> &tableNames) :
    Orch(db, tableNames) {}

/*
 * Purpose:     Perform the operations requested by APPL_DB users.
 *
 * Description: Redirect the operations to the appropriate table handling
 *              method.
 *
 * Params:      IN  consumer - The cosumer object.
 *
 * Returns:     Nothing.
 */
void CbfOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    string table_name = consumer.getTableName();

    if (table_name == APP_DSCP_TO_FC_MAP_TABLE_NAME)
    {
        doDscpTask(consumer);
    }
    else if (table_name == APP_EXP_TO_FC_MAP_TABLE_NAME)
    {
        doExpTask(consumer);
    }
}

/*
 * Purpose:     Perform the DSCP_TO_FC_MAP_TABLE operations.
 *
 * Description: Iterate over the untreated operations list and resolve them.
 *              The operations supported are SET and DEL.  If an operation
 *              could not be resolved, it will either remain in the list, or be
 *              removed, depending on the case.
 *
 * Params:      IN  consumer - The cosumer object.
 *
 * Returns:     Nothing.
 */
void CbfOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string dscp_map_name = kfvKey(t);
        string op = kfvOp(t);
        auto map_it = m_dscpMaps.find(dscp_map_name);
        bool success;

        if (op == SET_COMMAND)
        {
            SWSS_LOG_INFO("Set operator for DSCP_TO_FC map %s",
                             dscp_map_name.c_str());

            auto dscp_map_list = extractMap(t, DSCP);

            if (map_it == m_dscpMaps.end())
            {
                SWSS_LOG_NOTICE("Creating DSCP_TO_FC map %s",
                                 dscp_map_name.c_str());

                auto sai_oid = addMap(dscp_map_list, DSCP);

                if (sai_oid != SAI_NULL_OBJECT_ID)
                {
                    SWSS_LOG_INFO("Successfully created DSCP_TO_FC map");
                    m_dscpMaps.insert(dscp_map_name, sai_oid);
                    success = true;
                }
                else
                {
                    SWSS_LOG_ERROR("Failed to create DSCP_TO_FC map %s",
                                    dscp_map_name.c_str());
                    success = false;
                }
            }
            else
            {
                SWSS_LOG_NOTICE("Updating existing DSCP_TO_FC map %s",
                                 dscp_map_name.c_str());

                success = updateMap(map_it->second, dscp_map_list);

                if (success)
                {
                    SWSS_LOG_INFO("Successfully updated DSCP_TO_FC map");
                }
                else
                {
                    SWSS_LOG_ERROR("Failed to update DSCP_TO_FC map %s",
                                    dscp_map_name.c_str());
                }
            }

            // Remove the allocated list
            delete[] dscp_map_list.list;
        }
        else if (op == DEL_COMMAND)
        {
            if (map_it != m_dscpMaps.end())
            {
                SWSS_LOG_NOTICE("Deleting DSCP_TO_FC map %s",
                                 dscp_map_name.c_str());

                success = removeMap(map_it->second);

                if (success)
                {
                    SWSS_LOG_INFO("Successfully removed DSCP_TO_FC map");
                    m_dscpMaps.erase(map_it);
                }
                else
                {
                    SWSS_LOG_ERROR("Failed to remove DSCP_TO_FC map %s",
                                    dscp_map_name.c_str());
                }
            }
            else
            {
                SWSS_LOG_WARN("Tried to delete inexistent DSCP_TO_FC map %s",
                               dscp_map_name.c_str());
                // Mark it as success to remove it from the consumer
                success = true;
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
            // Set success to true to remove the operation from the consumer
            success = true;
        }

        if (success)
        {
            SWSS_LOG_DEBUG("Removing consumer item");
            it = consumer.m_toSync(erase(it));
        }
        else
        {
            SWSS_LOG_DEBUG("Keeping consumer item");
            ++it;
        }
    }
}

/*
 * Purpose:     Perform the EXP_TO_FC_MAP_TABLE operations.
 *
 * Description: Iterate over the untreated operations list and resolve them.
 *              The operations supported are SET and DEL.  If an operation
 *              could not be resolved, it will either remain in the list, or be
 *              removed, depending on the case.
 *
 * Params:      IN  consumer - The cosumer object.
 *
 * Returns:     Nothing.
 */
void CbfOrch::doExpTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string index = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {

        }
        else if (op == DEL_COMMAND)
        {

        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

sai_qos_map_list_t CbfOrch::extractMap(const KeyOpFieldsValuesTuple &t,
                                       MapType type)
{
    SWSS_LOG_ENTER();

    sai_qos_map_list_t map_list;
    const auto &fields_values = kfvFieldsValues(t);
    map_list.count = (uint32_t)fields_values.size();
    map_list.list = new sai_qos_map_t[map_list.count]();

    uint32_t ind = 0;
    for (auto i = fields_values.begin();
            i != fields_values.end();
            i++, ind++)
    {
        switch (type)
        {
            case DSCP:
                map_list.list[ind].key.dscp = (uint8_t)stoi(fvField(*i));
                break;

            case EXP:
                map_list.list[ind].key.exp = (uint8_t)stoi(fvField(*i));
                break;

            default:
                SWSS_LOG_ERROR("Unknown CBF map type");
                assert(false);
        }

        map_list.list[ind].value.fc = (uint8_t)stoi(fvValue(*i));
    }

    return map_list;
}

void CbfOrch::addMap(const sai_qos_map_list_t &map_list, MapType type)
{
    SWSS_LOG_ENTER();

    vector<sai_attribute_t> map_attrs;

    sai_attribute_t map_attr;
    map_attr.id = SAI_QOS_MAP_ATTR_TYPE;
    map_attr.value.u32 = type == DSCP ?
                      SAI_QOS_MAP_TYPE_DSCP_TO_FC : SAI_QOS_MAP_TYPE_EXP_TO_FC;
    map_attrs.push_back(map_attr);

    map_attr.id = SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST;
    map_attr.value.qosmap.count = map_list.count;
    map_attr.value.qosmap.list = map_list.list;
    map_attrs.push_back(map_attr);

    sai_object_id_t sai_oid;
    auto sai_status = sai_qos_map_api->create_qos_map(
                                                    &sai_oid,
                                                    gSwitchId,
                                                    (uint32_t)map_attrs.size(),
                                                    map_attrs.data());
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create map. status: %d", sai_status);
        return SAI_NULL_OBJECT_ID;
    }

    return sai_oid;
}

bool CbfOrch::updateMap(sai_object_id_t sai_oid,
                        const sai_qos_map_list_t &map_list)
{
    SWSS_LOG_ENTER();

    assert(sai_oid != SAI_NULL_OBJECT_ID);
    sai_attribute_t map_attr;
    map_attr.id = SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST;
    map_attr.value.qosmap.count = map_list.count;
    map_attr.value.qosmap.list = map_list.list;

    auto sai_status = sai_qos_map_api->set_qos_map_attribute(sai_oid,
                                                             &map_attr);

    if (sai_status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to update map. status: %d", sai_status);
        return false;
    }

    return true;
}

bool CbfOrch::removeMap(sai_object_id_t sai_oid)
{
    SWSS_LOG_ENTER();

    assert(sai_oid != SAI_NULL_OBJECT_ID);
    auto sai_status = sai_qos_map_api->remove_qos_map(sai_oid);

    if (sai_status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove map. status: %d", sai_status);
        return false;
    }

    return true;
}

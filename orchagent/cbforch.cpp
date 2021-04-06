#include "cbforch.h"

extern sai_qos_map_api_t *sai_qos_map_api;
extern sai_object_id_t gSwitchId;

/*
 * Purpose:     Constructor.
 *
 * Description: Initialize Orch base and members.
 *
 * Params:      IN  db         - The DB connection to Redis.
 *              IN  tableNames - The Redis tables to handle.
 *
 * Returns:     Nothing.
 */
CbfOrch::CbfOrch(DBConnector *db,
                 const vector<table_name_with_pri_t> &tableNames) :
    Orch(db, tableNames),
    m_dscp_map(MapHandler::DSCP),
    m_exp_map(MapHandler::EXP)
{
    SWSS_LOG_ENTER();
}

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
        m_dscp_map.doTask(consumer);
    }
    else if (table_name == APP_EXP_TO_FC_MAP_TABLE_NAME)
    {
        m_exp_map.doTask(consumer);
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
void MapHandler::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string map_id = kfvKey(t);
        string op = kfvOp(t);
        auto map_it = find(map_id);
        bool success;

        if (op == SET_COMMAND)
        {
            SWSS_LOG_INFO("Set operator for %s map %s",
                           getMapName(),
                           map_id.c_str());

            auto map_list = extractMap(t);

            if (map_it == end())
            {
                SWSS_LOG_NOTICE("Creating %s map %s",
                                 getMapName(),
                                 map_id.c_str());

                auto sai_oid = createMap(map_list);

                if (sai_oid != SAI_NULL_OBJECT_ID)
                {
                    SWSS_LOG_INFO("Successfully created %s map", getMapName());
                    emplace(map_id, sai_oid);
                    success = true;
                }
                else
                {
                    SWSS_LOG_ERROR("Failed to create %s map %s",
                                    getMapName(),
                                    map_id.c_str());
                    success = false;
                }
            }
            else
            {
                SWSS_LOG_NOTICE("Updating existing %s map %s",
                                 getMapName(),
                                 map_id.c_str());

                success = updateMap(map_it->second, map_list);

                if (success)
                {
                    SWSS_LOG_INFO("Successfully updated %s map", getMapName());
                }
                else
                {
                    SWSS_LOG_ERROR("Failed to update %s map %s",
                                    getMapName(),
                                    map_id.c_str());
                }
            }

            // Remove the allocated list
            delete[] map_list.list;
        }
        else if (op == DEL_COMMAND)
        {
            if (map_it != end())
            {
                SWSS_LOG_NOTICE("Deleting %s map %s",
                                 getMapName(),
                                 map_id.c_str());

                success = removeMap(map_it->second);

                if (success)
                {
                    SWSS_LOG_INFO("Successfully removed %s map", getMapName());
                    erase(map_it);
                }
                else
                {
                    SWSS_LOG_ERROR("Failed to remove %s map %s",
                                    getMapName(),
                                    map_id.c_str());
                }
            }
            else
            {
                SWSS_LOG_WARN("Tried to delete inexistent %s map %s",
                               getMapName(),
                               map_id.c_str());
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
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_DEBUG("Keeping consumer item");
            ++it;
        }
    }
}

/*
 * Purpose:     Extract the QoS map from the Redis's field-value pairs.
 *
 * Description: Iterate over the field-value pairs and create a SAI QoS map.
 *
 * Params:      IN  t - The field-value pairs.
 *
 * Returns:     The SAI QoS map object.
 */
sai_qos_map_list_t MapHandler::extractMap(const KeyOpFieldsValuesTuple &t)
                                                                          const
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
        if (m_type == DSCP)
        {
            map_list.list[ind].key.dscp = (uint8_t)stoi(fvField(*i));
        }
        else
        {
            map_list.list[ind].key.mpls_exp = (uint8_t)stoi(fvField(*i));
        }

        map_list.list[ind].value.fc = (uint8_t)stoi(fvValue(*i));
    }

    return map_list;
}

/*
 * Purpose:     Create a QoS map.
 *
 * Description: Create the QoS map over the SAI interface.
 *
 * Params:      IN  map_list - The QoS map to create.
 *
 * Returns:     The SAI ID for the newly created object or SAI_NULL_OBJECt_ID
 *              if something goes wrong.
 */
sai_object_id_t MapHandler::createMap(const sai_qos_map_list_t &map_list)
{
    SWSS_LOG_ENTER();

    vector<sai_attribute_t> map_attrs;

    sai_attribute_t map_attr;
    map_attr.id = SAI_QOS_MAP_ATTR_TYPE;
    map_attr.value.u32 = m_type == DSCP ?
                 SAI_QOS_MAP_TYPE_DSCP_TO_FC : SAI_QOS_MAP_TYPE_MPLS_EXP_TO_FC;
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

/*
 * Purpose:     Update a QoS map.
 *
 * Description: Update a QoS map over the SAI interface.
 *
 * Params:      IN  sai_oid  - The SAI ID of the object to update.
 *              IN  map_list - The QoS map to update the object with.
 *
 * Returns:     true,  if the update was successful,
 *              false, otherwise
 */
bool MapHandler::updateMap(sai_object_id_t sai_oid,
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

/*
 * Purpose:     Delete a QoS map.
 *
 * Description: Delete a QoS map over the SAI interface.
 *
 * Params:      IN  sai_oid - The SAI ID of the object to delete.
 *
 * Returns:     true,  if the delete was successful,
 *              false, otherwise
 */
bool MapHandler::removeMap(sai_object_id_t sai_oid)
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

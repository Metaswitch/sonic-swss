#include "cbfnhgorch.h"
#include "crmorch.h"

extern sai_object_id_t gSwitchId;

extern NhgOrch *gNhgOrch;
extern CrmOrch *gCrmOrch;

extern sai_next_hop_group_api_t* sai_next_hop_group_api;

#define FC_MAX_VAL 63

CbfNhgOrch::CbfNhgOrch(DBConnector *db, const std::string &tableName) :
    Orch(db, tableName)
{
    SWSS_LOG_ENTER();
}

/*
 * Purpose:     Perform the operations requested by APPL_DB users.
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
void CbfNhgOrch::doTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        std::string index = kfvKey(t);
        std::string op = kfvOp(t);

        SWSS_LOG_INFO("CBF next hop group key %s, op %s",
                       index.c_str(), op.c_str());

        bool success;
        const auto &cbf_nhg_it = m_syncdCbfNhgs.find(index);

        if (op == SET_COMMAND)
        {
            std::string members;
            std::string class_map;

            /*
             * Get CBF group's members and class map.
             */
            for (const auto &i : kfvFieldsValues(t))
            {
                if (fvField(i) == "members")
                {
                    members = fvValue(i);
                }
                else if (fvField(i) == "class_map")
                {
                    class_map = fvValue(i);
                }
            }

            SWSS_LOG_INFO("CBF NHG has members %s, class map %s",
                          members.c_str(), class_map.c_str());

            /*
             * Validate the data.
             */
            auto t = validateData(members, class_map);

            if (!std::get<0>(t))
            {
                SWSS_LOG_ERROR("CBF next hop group %s data is invalid.",
                               index.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /*
             * If the CBF group does not exist, create it.
             */
            if (cbf_nhg_it == m_syncdCbfNhgs.end())
            {
                SWSS_LOG_INFO("Creating the CBF next hop group");

                /*
                 * If we reached the NHG limit, postpone the creation.
                 */
                if (NextHopGroup::getCount() >= gNhgOrch->getMaxNhgCount())
                {
                    SWSS_LOG_WARN("Reached next hop group limit. Postponing "
                                  "creation.");
                    success = false;
                }
                else
                {
                    auto cbf_nhg = CbfNextHopGroup(index,
                                                   std::get<1>(t),
                                                   std::get<2>(t));
                    success = cbf_nhg.sync();

                    if (success)
                    {
                        SWSS_LOG_INFO("CBF NHG successfully synced.");
                        m_syncdCbfNhgs.emplace(index,
                                              CbfNhgEntry(std::move(cbf_nhg)));
                    }
                }

            }
            /*
             * If the CBF group exists, update it.
             */
            else
            {
                SWSS_LOG_INFO("Updating the CBF next hop group");
                success = cbf_nhg_it->second.cbf_nhg.update(std::get<1>(t),
                                                            std::get<2>(t));
            }
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_INFO("Deleting CBF next hop group %s", index.c_str());

            /*
             * If the group doesn't exist, do nothing.
             */
            if (cbf_nhg_it == m_syncdCbfNhgs.end())
            {
                SWSS_LOG_WARN("Deleting inexistent CBF NHG %s", index.c_str());
                /*
                 * Mark it as a success to remove the task from the consumer.
                 */
                success = true;
            }
            /*
             * If the group does exist but is still referenced, skip.
             */
            else if (cbf_nhg_it->second.ref_count > 0)
            {
                SWSS_LOG_WARN("Skipping removal of CBF next hop group %s which"
                              " is still referenced", index.c_str());
                success = false;
            }
            /*
             * Otherwise, delete it.
             */
            else
            {
                SWSS_LOG_INFO("Removing CBF next hop group");
                success = cbf_nhg_it->second.cbf_nhg.desync();

                if (success)
                {
                    SWSS_LOG_INFO("Successfully desynced CBF next hop group");
                    m_syncdCbfNhgs.erase(cbf_nhg_it);
                }
            }
        }
        else
        {
            SWSS_LOG_WARN("Unknown operation type %s", op.c_str());
            /*
             * Mark the operation as a success to remove the task.
             */
            success = true;
        }

        /*
         * Depending on the operation success, remove the task or skip it.
         */
        if (success)
        {
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

std::tuple<bool,
           std::unordered_set<std::string>,
           std::unordered_map<uint8_t, uint8_t>>
                         CbfNhgOrch::validateData(const std::string &members,
                                                  const std::string &class_map)
{
    SWSS_LOG_ENTER();

    auto members_vec = tokenize(members, ',');
    auto class_map_vec = tokenize(class_map, ',');

    /*
     * Verify that the members and class map are not empty and are of
     * the same size.
     */
    if (members_vec.empty() ||
        class_map_vec.empty() ||
        (members_vec.size() != class_map_vec.size()))
    {
        SWSS_LOG_INFO("CBF NHG data is empty or miss-matched in size.");
        return std::make_tuple(false,
                               std::unordered_set<std::string>(),
                               std::unordered_map<uint8_t, uint8_t>());
    }

    /*
     * Verify that the members are unique.
     */
    std::unordered_set<std::string> members_set(members_vec.begin(),
                                                members_vec.end());
    if (members_set.size() != members_vec.size())
    {
        SWSS_LOG_INFO("CBF NHG members are not unique.");
        return std::make_tuple(false,
                               std::unordered_set<std::string>(),
                               std::unordered_map<uint8_t, uint8_t>());
    }

    /*
     * Verify that the class map contains valid data. The FC should be
     * between 0 and 63 (inclusive), and the index should be between
     * 1 and the size of members (inclusive). Also, the FC values
     * should be unique (the same FC can't be mapped more than once).
     */
    std::unordered_map<uint8_t, uint8_t> class_map_map;

    for (const auto &i : class_map_vec)
    {
        /*
         * Check that the mapping is correctly formed.
         */
        auto tokens = tokenize(i, ':');

        if (tokens.size() != 2)
        {
            SWSS_LOG_INFO("CBF NHG class map is ill-formed");
            return std::make_tuple(false,
                                   std::unordered_set<std::string>(),
                                   std::unordered_map<uint8_t, uint8_t>());
        }

        try
        {
            /*
             * Check that the FC value is valid.
             */
            auto fc = std::stoi(tokens[0]);

            if ((fc < 0) || (fc > FC_MAX_VAL))
            {
                SWSS_LOG_INFO("CBF NHG class map contains invalid FC %d", fc);
                return std::make_tuple(false,
                                       std::unordered_set<std::string>(),
                                       std::unordered_map<uint8_t, uint8_t>());
            }

            /*
             * Check that the index value is valid.
             */
            auto index = std::stoi(tokens[1]);

            if ((index < 1) || (index > members_vec.size()))
            {
                SWSS_LOG_INFO("CBF NHG class map contains invalid index %d",
                              index);
                return std::make_tuple(false,
                                       std::unordered_set<std::string>(),
                                       std::unordered_map<uint8_t, uint8_t>());
            }

            /*
             * Check that the mapping is unique.
             */
            auto rc = class_map_map.emplace(static_cast<uint8_t>(fc),
                                        static_cast<uint8_t>(index)).second;
            if (!rc)
            {
                SWSS_LOG_INFO("CBF NHG class map maps FC %d more than once",
                              fc);
                return std::make_tuple(false,
                                       std::unordered_set<std::string>(),
                                       std::unordered_map<uint8_t, uint8_t>());
            }
        }
        catch(const std::exception& e)
        {
            SWSS_LOG_INFO("Failed to convert CBF NHG FC or index to uint8_t.");
            return std::make_tuple(false,
                                   std::unordered_set<std::string>(),
                                   std::unordered_map<uint8_t, uint8_t>());
        }
    }

    return std::make_tuple(true, members_set, class_map_map);
}

CbfNextHopGroup::CbfNextHopGroup(
                       const std::string &index,
                       const std::unordered_set<std::string> &members,
                       const std::unordered_map<uint8_t, uint8_t> &class_map) :
    m_key(index),
    m_members(members),
    m_class_map(class_map)
{
    SWSS_LOG_ENTER();
}

CbfNextHopGroup::CbfNextHopGroup(CbfNextHopGroup &&cbf_nhg) :
    m_key(std::move(cbf_nhg.m_key)),
    m_members(std::move(cbf_nhg.m_members)),
    m_class_map(std::move(cbf_nhg.m_class_map))
{
    SWSS_LOG_ENTER();
}

bool CbfNextHopGroup::sync()
{
    SWSS_LOG_ENTER();

    /*
     * Create the CBF next hop group over SAI.
     */
    sai_attribute_t nhg_attr;
    std::vector<sai_attribute_t> nhg_attrs;

    nhg_attr.id = SAI_NEXT_HOP_GROUP_ATTR_TYPE;
    nhg_attr.value.s32 = SAI_NEXT_HOP_GROUP_TYPE_CLASS_BASED;
    nhg_attrs.push_back(nhg_attr);

    /*
     * Add the class map to the attributes.
     */
    nhg_attrs.push_back(getClassMapAttr());

    auto status = sai_next_hop_group_api->create_next_hop_group(
                                    &m_id,
                                    gSwitchId,
                                    static_cast<uint32_t>(nhg_attrs.size()),
                                    nhg_attrs.data());

    /*
     * Free the class map attribute resources.
     */
    delete[] nhg_attrs[1].value.maplist.list;

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create CBF next hop group %s, rv %d",
                        m_key.c_str(),
                        status);
        return false;
    }

    /*
     * Increment the amount of programmed next hop groups.
     */
    gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);
    ++m_count;

    return true;
}

/*
 * Purpose: Desync a CBF next hop group.
 *
 * Params:  None.
 *
 * Returns: true, if the desync was successful,
 *          false, otherwise.
 */
bool CbfNextHopGroup::desync()
{
    SWSS_LOG_ENTER();

    /*
     * If the group is already desynced, there is nothing to be done.
     */
    if (!isSynced())
    {
        SWSS_LOG_INFO("CBF next hop group is already desynced");
        return true;
    }

    /*
     * Remove the CBF NHG over SAI.
     */
    auto status = sai_next_hop_group_api->remove_next_hop_group(m_id);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove CBF next hop group %s, rv: %d",
                        m_key.c_str(), status);
        return false;
    }

    /*
     * Decrease the number of programmed NHGs.
     */
    gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);
    --m_count;

    return true;
}

/*
 * Purpose: Update a CBF next hop group.
 *
 * Params:  IN members - The new members.
 *          IN class_map - The new class map.
 *
 * Returns: true, if the update was successful,
 *          false, otherwise.
 */
bool CbfNextHopGroup::update(const std::unordered_set<std::string> &members,
                        const std::unordered_map<uint8_t, uint8_t> &class_map)
{
    SWSS_LOG_ENTER();

    m_members = members;
    m_class_map = class_map;

    sai_attribute_t nhg_attr = getClassMapAttr();

    auto status = sai_next_hop_group_api->set_next_hop_group_attribute(m_id,
                                                                    &nhg_attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to update CBF next hop group %s, rv %d",
                        m_key.c_str(),
                        status);
        return false;
    }

    return true;
}

/*
 * Purpose: Build the SAI attribute for the class map.  The caller is
 *          responsible for freeing the resources when finished using the
 *          attribute.
 *
 * Params:  None.
 *
 * Returns: The SAI attribute.
 */
sai_attribute_t CbfNextHopGroup::getClassMapAttr() const
{
    SWSS_LOG_ENTER();

    sai_attribute_t nhg_attr;
    nhg_attr.id = SAI_NEXT_HOP_GROUP_ATTR_FORWARDING_CLASS_TO_INDEX_MAP;
    nhg_attr.value.maplist.count = static_cast<uint32_t>(m_class_map.size());
    nhg_attr.value.maplist.list =
                                new sai_map_t[nhg_attr.value.maplist.count]();
    uint32_t idx = 0;
    for (const auto &it : m_class_map)
    {
        nhg_attr.value.maplist.list[idx].key = it.first;
        nhg_attr.value.maplist.list[idx++].value = it.second;
    }

    return nhg_attr;
}

CbfNhgEntry::CbfNhgEntry(CbfNextHopGroup &&cbf_nhg) :
    cbf_nhg(std::move(cbf_nhg)),
    ref_count(0)
{
    SWSS_LOG_ENTER();
}

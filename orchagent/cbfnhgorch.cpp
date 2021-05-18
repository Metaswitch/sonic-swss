#include "cbfnhgorch.h"
#include "crmorch.h"
#include "bulker.h"
#include "tokenize.h"
#include "nhgorch.h"

extern sai_object_id_t gSwitchId;

extern NhgOrch *gNhgOrch;
extern CrmOrch *gCrmOrch;

extern sai_next_hop_group_api_t* sai_next_hop_group_api;

#define FC_MAX_VAL 63

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
        swss::KeyOpFieldsValuesTuple t = it->second;

        string index = kfvKey(t);
        string op = kfvOp(t);

        SWSS_LOG_INFO("CBF next hop group key %s, op %s",
                       index.c_str(), op.c_str());

        bool success;
        const auto &cbf_nhg_it = m_syncedNhgs.find(index);

        if (op == SET_COMMAND)
        {
            string members;
            string class_map;

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

            if (!get<0>(t))
            {
                SWSS_LOG_ERROR("CBF next hop group %s data is invalid.",
                               index.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /*
             * If the CBF group does not exist, create it.
             */
            if (cbf_nhg_it == m_syncedNhgs.end())
            {
                SWSS_LOG_INFO("Creating the CBF next hop group");

                /*
                 * If we reached the NHG limit, postpone the creation.
                 */
                if (NhgBase::getSyncedCount() >= NhgOrch::getMaxNhgCount())
                {
                    SWSS_LOG_WARN("Reached next hop group limit. Postponing "
                                  "creation.");
                    success = false;
                }
                else
                {
                    auto cbf_nhg = CbfNextHopGroup(index,
                                                   get<1>(t),
                                                   get<2>(t));
                    success = cbf_nhg.sync();

                    if (success)
                    {
                        SWSS_LOG_INFO("CBF NHG successfully synced.");
                        m_syncedNhgs.emplace(index,
                                    NhgEntry<CbfNextHopGroup>(move(cbf_nhg)));
                    }
                }

            }
            /*
             * If the CBF group exists, update it.
             */
            else
            {
                SWSS_LOG_INFO("Updating the CBF next hop group");
                success = cbf_nhg_it->second.nhg.update(get<1>(t), get<2>(t));
            }
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_INFO("Deleting CBF next hop group %s", index.c_str());

            /*
             * If the group doesn't exist, do nothing.
             */
            if (cbf_nhg_it == m_syncedNhgs.end())
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
                success = cbf_nhg_it->second.nhg.desync();

                if (success)
                {
                    SWSS_LOG_INFO("Successfully desynced CBF next hop group");
                    m_syncedNhgs.erase(cbf_nhg_it);
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

tuple<bool, set<string>, unordered_map<uint8_t, uint8_t>>
    CbfNhgOrch::validateData(const string &members, const string &class_map)
{
    SWSS_LOG_ENTER();

    auto members_vec = swss::tokenize(members, ',');
    auto class_map_vec = swss::tokenize(class_map, ',');

    /*
     * Verify that the members and class map are not empty and are of
     * the same size.
     */
    if (members_vec.empty() ||
        class_map_vec.empty() ||
        (members_vec.size() != class_map_vec.size()))
    {
        SWSS_LOG_INFO("CBF NHG data is empty or miss-matched in size.");
        return make_tuple(false,
                               set<string>(),
                               unordered_map<uint8_t, uint8_t>());
    }

    /*
     * Verify that the members are unique.
     */
    set<string> members_set(members_vec.begin(),
                                                members_vec.end());
    if (members_set.size() != members_vec.size())
    {
        SWSS_LOG_INFO("CBF NHG members are not unique.");
        return make_tuple(false,
                               set<string>(),
                               unordered_map<uint8_t, uint8_t>());
    }

    /*
     * Verify that the class map contains valid data. The FC should be
     * between 0 and 63 (inclusive), and the index should be between
     * 1 and the size of members (inclusive). Also, the FC values
     * should be unique (the same FC can't be mapped more than once).
     */
    unordered_map<uint8_t, uint8_t> class_map_map;

    for (const auto &i : class_map_vec)
    {
        /*
         * Check that the mapping is correctly formed.
         */
        auto tokens = swss::tokenize(i, ':');

        if (tokens.size() != 2)
        {
            SWSS_LOG_INFO("CBF NHG class map is ill-formed");
            return make_tuple(false,
                                   set<string>(),
                                   unordered_map<uint8_t, uint8_t>());
        }

        try
        {
            /*
             * Check that the FC value is valid.
             */
            auto fc = stoi(tokens[0]);

            if ((fc < 0) || (fc > FC_MAX_VAL))
            {
                SWSS_LOG_INFO("CBF NHG class map contains invalid FC %d", fc);
                return make_tuple(false,
                                       set<string>(),
                                       unordered_map<uint8_t, uint8_t>());
            }

            /*
             * Check that the index value is valid.
             */
            auto index = stoi(tokens[1]);

            if ((index < 0) || (index >= (int)members_vec.size()))
            {
                SWSS_LOG_INFO("CBF NHG class map contains invalid index %d",
                              index);
                return make_tuple(false,
                                       set<string>(),
                                       unordered_map<uint8_t, uint8_t>());
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
                return make_tuple(false,
                                       set<string>(),
                                       unordered_map<uint8_t, uint8_t>());
            }
        }
        catch(const exception& e)
        {
            SWSS_LOG_INFO("Failed to convert CBF NHG FC or index to uint8_t.");
            return make_tuple(false,
                                   set<string>(),
                                   unordered_map<uint8_t, uint8_t>());
        }
    }

    return make_tuple(true, members_set, class_map_map);
}

CbfNextHopGroup::CbfNextHopGroup(
                       const string &index,
                       const set<string> &members,
                       const unordered_map<uint8_t, uint8_t> &class_map) :
    NhgCommon(index),
    m_class_map(class_map)
{
    SWSS_LOG_ENTER();

    for (const auto &member : members)
    {
        m_members.emplace(member, CbfNhgMember(member));
    }
}

CbfNextHopGroup::CbfNextHopGroup(CbfNextHopGroup &&cbf_nhg) :
    NhgCommon(move(cbf_nhg)),
    m_class_map(move(cbf_nhg.m_class_map))
{
    SWSS_LOG_ENTER();
}

bool CbfNextHopGroup::sync()
{
    SWSS_LOG_ENTER();

    /*
     * If the group is already synced, exit.
     */
    if (isSynced())
    {
        SWSS_LOG_INFO("Group %s is already synced", m_key.c_str());
        return true;
    }

    /*
     * Create the CBF next hop group over SAI.
     */
    sai_attribute_t nhg_attr;
    vector<sai_attribute_t> nhg_attrs;

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
    incSyncedCount();

    /*
     * Sync the group members.
     */
    set<string> members;

    for (const auto &member : m_members)
    {
        members.insert(member.first);
    }

    if (!syncMembers(members))
    {
        SWSS_LOG_ERROR("Failed to sync CBF next hop group %s", m_key.c_str());
        return false;
    }

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
     * Desync the group members.
     */
    set<string> members;

    for (const auto &member : m_members)
    {
        members.insert(member.first);
    }

    if (!desyncMembers(members))
    {
        SWSS_LOG_ERROR("Failed to desync CBF next hop group %s members",
                        m_key.c_str());
        return false;
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
    decSyncedCount();

    /*
     * Reset the group ID.
     */
    m_id = SAI_NULL_OBJECT_ID;

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
bool CbfNextHopGroup::update(const set<string> &members,
                        const unordered_map<uint8_t, uint8_t> &class_map)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_INFO("Updating CBF next hop group %s", m_key.c_str());

    /*
     * Update the group members.
     */
    set<string> removed_members;

    /*
     * Store the members that need to be removed.
     */
    for (const auto &member : m_members)
    {
        /*
         * If the current member doesn't exist in the new set of members, it
         * was removed.
         */
        const auto &it = members.find(member.first);
        if (it == members.end())
        {
            SWSS_LOG_INFO("CBF next hop group member %s was removed",
                            member.first.c_str());
            removed_members.insert(member.first);
        }
    }

    /*
     * Desync the removed members.
     */
    if (!desyncMembers(removed_members))
    {
        SWSS_LOG_ERROR("Failed to desync members of CBF next hop group %s",
                        m_key.c_str());
        return false;
    }

    /*
     * Erase the desynced members.
     */
    for (const auto &removed_member : removed_members)
    {
        SWSS_LOG_DEBUG("Erasing removed next hop group member %s",
                        removed_member.c_str());
        m_members.erase(removed_member);
    }

    /*
     * Add anny new members to the group.
     */
    for (const auto &new_member : members)
    {
        SWSS_LOG_INFO("Adding next hop group member %s to the group",
                        new_member.c_str());
        m_members.emplace(new_member, CbfNhgMember(new_member));
    }

    /*
     * Sync all the members of the group.  We sync all of them as index changes
     * might occur and we need to update those members.
     */
    if (!syncMembers(members))
    {
        SWSS_LOG_ERROR("Failed to sync members of CBF next hop group %s",
                        m_key.c_str());
        return false;
    }

    /*
     * Update the group map.
     */
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
 * Purpose: Sync the given CBF group members.
 *
 * Params:  IN members - The members to sync.
 *
 * Returns: true, if the operation was successful,
 *          false, otherwise.
 */
bool CbfNextHopGroup::syncMembers(const set<string> &members)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_INFO("Syncing CBF next hop group %s members", m_key.c_str());

    /*
     * The group should be synced at this point.
     */
    if (!isSynced())
    {
        SWSS_LOG_ERROR("Trying to sync members of CBF next hop group %s which"
                        " is not synced", m_key.c_str());
        throw logic_error("Syncing members of unsynced CBF next hop "
                                "group");
    }

    /*
     * Sync all the given members.  If a NHG does not exist, is not yet synced
     * or is temporary, stop immediately.
     */
    ObjectBulker<sai_next_hop_group_api_t> bulker(sai_next_hop_group_api,
                                                    gSwitchId);
    unordered_map<string, sai_object_id_t> nhgm_ids;
    uint8_t idx = 0;

    for (const auto &key : members)
    {
        SWSS_LOG_INFO("Checking next hop group %s", key.c_str());

        auto &nhgm = m_members.at(key);

        /*
         * If the member is already synced, it means an update occurred for
         * which we need to update the member's index.
         */
        if (nhgm.isSynced())
        {
            SWSS_LOG_INFO("Updating CBF next hop group %s index attribute",
                            nhgm.to_string().c_str());

            if (!nhgm.setIndex(idx++))
            {
                SWSS_LOG_ERROR("Failed to update next hop group member %s of "
                                " CBF next hop group %s index value",
                                nhgm.to_string().c_str(), m_key.c_str());
                return false;
            }
            continue;
        }

        /*
         * Check if the group exists in NhgOrch.
         */
        if (!gNhgOrch->nonCbfNhgOrch.hasNhg(key))
        {
            SWSS_LOG_ERROR("Next hop group %s in CBF next hop group %s does "
                            "not exist", key.c_str(), m_key.c_str());
            return false;
        }

        const auto &nhg = gNhgOrch->nonCbfNhgOrch.getNhg(key);

        /*
         * Check if the group is synced.
         */
        if (!nhg.isSynced() || nhg.isTemp())
        {
            SWSS_LOG_ERROR("Next hop group %s in CBF next hop group %s is not"
                            " synced or it's temporary",
                            key.c_str(), m_key.c_str());
            return false;
        }

        /*
         * Create the SAI attributes for syncing the NHG as a member.
         */
        auto attrs = createNhgmAttrs(nhgm);

        sai_attribute_t attr;
        attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_INDEX;
        attr.value.oid = idx;
        attrs.push_back(attr);

        /*
         * Set the member's index.
         */
        nhgm.setIndex(idx++);

        bulker.create_entry(&nhgm_ids[key],
                            (uint32_t)attrs.size(),
                            attrs.data());
    }

    /*
     * Flush the bulker to perform the sync.
     */
    bulker.flush();

    /*
     * Iterate over the synced members and set their SAI ID.
     */
    bool success = true;

    for (const auto &member : nhgm_ids)
    {
        SWSS_LOG_DEBUG("CBF next hop group member %s has SAI ID %lu",
                        member.first.c_str(), member.second);

        if (member.second == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_ERROR("Failed to create CBF next hop group %s member %s",
                            m_key.c_str(), member.first.c_str());
            success = false;
        }
        else
        {
            SWSS_LOG_DEBUG("Successfully synced CBF next hop group member %s",
                            member.first.c_str());
            m_members.at(member.first).sync(member.second);
        }
    }

    SWSS_LOG_DEBUG("Returning %d", success);

    return success;
}

/*
 * Purpose: Desync the given CBF next hop group members.
 *
 * Params:  IN members - The members to desync.
 *
 * Returns: true, if the operation was successful,
 *          false, otherwise.
 */
bool CbfNextHopGroup::desyncMembers(const set<string> &members)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_INFO("Desyincing members of CBF next hop group %s",
                    m_key.c_str());

    /*
     * Desync all the given members from the group.
     */
    ObjectBulker<sai_next_hop_group_api_t> bulker(sai_next_hop_group_api,
                                                    gSwitchId);
    unordered_map<string, sai_status_t> statuses;

    for (const auto &key : members)
    {
        SWSS_LOG_INFO("Desyncing next hop group member %s", key.c_str());

        const auto &nhgm = m_members.at(key);

        if (nhgm.isSynced())
        {
            SWSS_LOG_DEBUG("Next hop group member %s is synced", key.c_str());
            bulker.remove_entry(&statuses[key], nhgm.getId());
        }
    }

    /*
     * Flush the bulker to desync the members.
     */
    bulker.flush();

    /*
     * Iterate over the returned statuses and check if the removal was
     * successful.  If it was, desync the member, otherwise log an error
     * message.
     */
    bool success = true;

    for (const auto &status : statuses)
    {
        SWSS_LOG_DEBUG("Verifying CBF next hop group member %s status",
                        status.first.c_str());

        if (status.second == SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_DEBUG("CBF next hop group member was successfully "
                            "desynced");
            m_members.at(status.first).desync();
        }
        else
        {
            SWSS_LOG_ERROR("Failed to desync CBF next hop group member %s, "
                            "rv: %d", status.first.c_str(), status.second);
            success = false;
        }
    }

    SWSS_LOG_DEBUG("Returning %d", success);

    return success;
}

/*
 * Purpose: Create a vector with the SAI attributes for syncing a next hop
 *          group member over SAI.  The caller is reponsible of filling in the
 *          index attribute.
 *
 * Params:  IN nhgm - The next hop group member to sync.
 *
 * Returns: The vector containing the SAI attributes.
 */
vector<sai_attribute_t>
            CbfNextHopGroup::createNhgmAttrs(const CbfNhgMember &nhgm) const
{
    SWSS_LOG_ENTER();

    if (!isSynced() || (nhgm.getNhgId() == SAI_NULL_OBJECT_ID))
    {
        SWSS_LOG_ERROR("CBF next hop group %s or next hop group %s are not "
                        "synced", m_key.c_str(), nhgm.to_string().c_str());
        throw logic_error("CBF next hop group member attributes data is "
                                "insufficient");
    }

    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;

    /*
     * Fill in the group ID.
     */
    attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
    attr.value.oid = m_id;
    attrs.push_back(attr);

    /*
     * Fill in the next hop ID.
     */
    attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
    attr.value.oid = nhgm.getNhgId();
    attrs.push_back(attr);

    return attrs;
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

/*
 * Purpose: Sync the member, setting its SAI ID and incrementing the necessary
 *          ref counters.
 *
 * Params:  IN gm_id - The SAI ID to set.
 *
 * Returns: Nothing.
 */
void CbfNhgMember::sync(sai_object_id_t gm_id)
{
    SWSS_LOG_ENTER();

    NhgMember::sync(gm_id);
    gNhgOrch->nonCbfNhgOrch.incNhgRefCount(m_key);
}

/*
 * Purpose: Desync the member, reseting its SAI ID and decrementing the NHG ref
 *          counter.
 *
 * Params:  None.
 *
 * Returns: Nothing.
 */
void CbfNhgMember::desync()
{
    SWSS_LOG_ENTER();

    NhgMember::desync();
    gNhgOrch->nonCbfNhgOrch.decNhgRefCount(m_key);
}

/*
 * Purpose: Get the NHG ID of this member.
 *
 * Params:  None.
 *
 * Returns: The SAI ID of the NHG it references or SAI_NULL_OBJECT_ID if it
 *          doesn't exist.
 */
sai_object_id_t CbfNhgMember::getNhgId() const
{
    SWSS_LOG_ENTER();

    if (!gNhgOrch->nonCbfNhgOrch.hasNhg(m_key))
    {
        SWSS_LOG_INFO("NHG %s does not exist", to_string().c_str());
        return SAI_NULL_OBJECT_ID;
    }

    return gNhgOrch->nonCbfNhgOrch.getNhg(m_key).getId();
}

/*
 * Purpose: Set the index of this member.
 *
 * Params:  IN index - The index to set.
 *
 * Returns: true, if the operation was successful,
 *          false, otherwise.
 */
bool CbfNhgMember::setIndex(uint8_t index)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_INFO("Updating CBF next hop group member %s index from %u to %u",
                    to_string().c_str(), m_index, index);

    if (m_index == index)
    {
        SWSS_LOG_DEBUG("The index is the same - exit");
        return true;
    }

    m_index = index;

    /*
     * If the member is synced, update it's attribute value.
     */
    if (isSynced())
    {
        SWSS_LOG_INFO("Updating SAI index attribute");

        sai_attribute_t attr;
        attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_INDEX;
        attr.value.oid = m_index;

        auto status = sai_next_hop_group_api->
                            set_next_hop_group_member_attribute(m_id, &attr);

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to update CBF next hop group member %s "
                            "index", to_string().c_str());
            return false;
        }
    }

    return true;
}

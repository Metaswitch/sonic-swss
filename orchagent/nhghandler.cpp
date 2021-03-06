#include "nhghandler.h"
#include "nhgorch.h"
#include "neighorch.h"
#include "crmorch.h"
#include "bulker.h"
#include "logger.h"
#include "swssnet.h"

extern sai_object_id_t gSwitchId;

extern NeighOrch *gNeighOrch;

extern sai_next_hop_group_api_t* sai_next_hop_group_api;
extern sai_next_hop_api_t*         sai_next_hop_api;

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
void NhgHandler::doTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string index = kfvKey(t);
        string op = kfvOp(t);

        SWSS_LOG_INFO("Next hop group table key %s, op %s",
                        index.c_str(), op.c_str());

        bool success;
        const auto& nhg_it = m_syncedNhgs.find(index);

        if (op == SET_COMMAND)
        {
            string ips;
            string aliases;
            string weights;

            /* Get group's next hop IPs and aliases */
            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "nexthop")
                    ips = fvValue(i);

                if (fvField(i) == "ifname")
                    aliases = fvValue(i);

                if (fvField(i) == "weight")
                    weights = fvValue(i);
            }

            /* Split ips and alaises strings into vectors of tokens. */
            vector<string> ipv = tokenize(ips, ',');
            vector<string> alsv = tokenize(aliases, ',');

            /* Create the next hop group key. */
            string nhg_str = ipv[0] + NH_DELIMITER + alsv[0];

            for (uint32_t i = 1; i < ipv.size(); i++)
            {
                nhg_str += NHG_DELIMITER + ipv[i] + NH_DELIMITER + alsv[i];
            }

            NextHopGroupKey nhg_key = NextHopGroupKey(nhg_str, weights);

            /* If the group does not exist, create one. */
            if (nhg_it == m_syncedNhgs.end())
            {
                SWSS_LOG_INFO("Adding next hop group %s with %s",
                                index.c_str(),
                                nhg_str.c_str());
                /*
                * If we'd have to create a SAI object for the group, and we
                * already reached the limit, we're going to create a temporary
                * group, represented by one of it's NH only until we have
                * enough resources to sync the whole group.  The item is going
                * to be kept in the sync list so we keep trying to create the
                * actual group when there are enough resources.
                */
                if ((nhg_key.getSize() > 1) &&
                    (Nhg::getSyncedCount() >= NhgOrch::getMaxNhgCount()))
                {
                    SWSS_LOG_WARN("Next hop group count reached it's limit.");

                    try
                    {
                        auto nhg = createTempNhg(nhg_key);
                        SWSS_LOG_INFO("Adding temp next hop group with %s",
                                        nhg.to_string().c_str());
                        if (nhg.sync())
                        {
                            SWSS_LOG_INFO("Temporary NHG successfully synced");
                            m_syncedNhgs.emplace(index,
                                        NhgEntry<Nhg>(std::move(nhg)));
                        }
                        else
                        {
                            SWSS_LOG_WARN(
                                "Failed to sync temporary NHG %s with %s",
                                index.c_str(),
                                nhg_key.to_string().c_str());
                        }
                    }
                    catch (const std::exception& e)
                    {
                        SWSS_LOG_WARN(
                            "Got exception: %s while adding temp group %s",
                            e.what(),
                            nhg_key.to_string().c_str());
                    }

                    /*
                     * We set the success to false so we keep trying to update
                     * this group in order to sync the whole group.
                     */
                    success = false;
                }
                else
                {
                    auto nhg = Nhg(nhg_key);
                    success = nhg.sync();

                    if (success)
                    {
                        SWSS_LOG_INFO("NHG successfully synced");
                        m_syncedNhgs.emplace(index,
                                        NhgEntry<Nhg>(std::move(nhg)));
                    }
                }
            }
            /* If the group exists, update it. */
            else
            {
                SWSS_LOG_INFO("Update next hop group %s with %s",
                                index.c_str(),
                                nhg_str.c_str());

                auto& nhg = nhg_it->second.nhg;

                /*
                 * A NHG update should never change the SAI ID of the NHG if it
                 * is still referenced by some other objects, as they would not
                 * be notified about this change.  The only exception to this
                 * rule is for the temporary NHGs, as the referencing objects
                 * will keep querying the NhgOrch for any SAI ID updates.
                 */
                if (!nhg.isTemp() &&
                    ((nhg_key.getSize() == 1) || (nhg.getSize() == 1)) &&
                    (nhg_it->second.ref_count > 0))
                {
                    SWSS_LOG_WARN("Next hop group %s update would change SAI "
                                  "ID while referenced, so not performed",
                                  index.c_str());
                    success = false;
                }
                /*
                 * If the update would mandate promoting a temporary next hop
                 * group to a multiple next hops group and we do not have the
                 * resources yet, we have to skip it until we have enough
                 * resources.
                 */
                else if (nhg.isTemp() &&
                         (nhg_key.getSize() > 1) &&
                         (Nhg::getSyncedCount() >= NhgOrch::getMaxNhgCount()))
                {
                    /*
                     * If the group was updated in such way that the previously
                     * chosen next hop does not represent the new group key,
                     * update the temporary group to choose a new next hop from
                     * the new key.
                     */
                    if (!nhg_key.contains(nhg.getKey()))
                    {
                        SWSS_LOG_NOTICE("Updating temporary group %s to %s",
                                        index.c_str(),
                                        nhg_key.to_string().c_str());

                        try
                        {
                            /* Create the new temporary next hop group. */
                            auto new_nhg = createTempNhg(nhg_key);

                            /*
                            * If we successfully sync the new group, update
                            * only the next hop group entry's pointer so we
                            * don't mess up the reference counter, as other
                            * objects may already reference it.
                            */
                            if (new_nhg.sync())
                            {
                                SWSS_LOG_INFO(
                                        "Temporary NHG successfully synced");
                                nhg_it->second.nhg = std::move(new_nhg);
                            }
                            else
                            {
                                SWSS_LOG_WARN(
                                  "Failed to sync updated temp NHG %s with %s",
                                  index.c_str(),
                                  nhg_key.to_string().c_str());
                            }
                        }
                        catch (const std::exception& e)
                        {
                            SWSS_LOG_WARN(
                                "Got exception: %s while adding temp group %s",
                                e.what(),
                                nhg_key.to_string().c_str());
                        }
                    }

                    /*
                     * Because the resources are exhausted, we have to keep
                     * trying to update the temporary group until we can
                     * promote it to a fully functional group.
                     */
                    success = false;
                }
                /* Common update, when all the requirements are met. */
                else
                {
                    success = nhg.update(nhg_key);
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_INFO("Deleting next hop group %s", index.c_str());

            /*
             * If there is a pending SET after this DEL operation, skip the
             * DEL operation to perform the update instead.  Otherwise, in the
             * scenario where the DEL operation may be blocked by the ref
             * counter, we'd end up deleting the object after the SET operation
             * is performed, which would not reflect the desired state of the
             * object.
             */
            if (consumer.m_toSync.count(it->first) > 1)
            {
                SWSS_LOG_INFO("There is a pending SET operation - skipping "
                                "delete operation");
                success = true;
            }
            /* If the group does not exist, do nothing. */
            else if (nhg_it == m_syncedNhgs.end())
            {
                SWSS_LOG_WARN("Unable to find group with key %s to remove",
                                index.c_str());
                /* Mark the operation as successful to consume it. */
                success = true;
            }
            /* If the group does exist, but it's still referenced, skip. */
            else if (nhg_it->second.ref_count > 0)
            {
                SWSS_LOG_WARN(
                            "Unable to remove group %s which is referenced",
                            index.c_str());
                success = false;
            }
            /* Else, if the group is no more referenced, delete it. */
            else
            {
                auto& nhg = nhg_it->second.nhg;

                success = nhg.desync();

                if (success)
                {
                    m_syncedNhgs.erase(nhg_it);
                }
            }
        }
        else
        {
            SWSS_LOG_WARN("Unknown operation type %s", op.c_str());
            /* Mark the operation as successful to consume it. */
            success = true;
        }

        /* Depending on the operation success, consume it or skip it. */
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

/*
 * Purpose:     Validate a next hop for any groups that contains it.
 *
 * Description: Iterate over all next hop groups and validate the next hop in
 *              those who contain it.
 *
 * Params:      IN  nh_key - The next hop to validate.
 *
 * Returns:     true, if the next hop was successfully validated in all
 *              containing groups;
 *              false, otherwise.
 */
bool NhgHandler::validateNextHop(const NextHopKey& nh_key)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_INFO("Validating next hop %s", nh_key.to_string().c_str());

    /*
     * Iterate through all groups and validate the next hop in those who
     * contain it.
     */
    for (auto& it : m_syncedNhgs)
    {
        auto& nhg = it.second.nhg;

        SWSS_LOG_INFO("Check if next hop in group %s",
                        it.first.c_str());

        if (nhg.hasMember(nh_key))
        {
            SWSS_LOG_INFO("Group has next hop");

            /*
             * If sync fails, exit right away, as we expect it to be due to a
             * raeson for which any other future validations will fail too.
             */
            if (!nhg.validateNextHop(nh_key))
            {
                SWSS_LOG_ERROR("Failed to validate next hop %s in group %s",
                                nh_key.to_string().c_str(),
                                it.first.c_str());
                return false;
            }
        }
    }

    return true;
}

/*
 * Purpose:     Invalidate a next hop for any groups containing it.
 *
 * Description: Iterate through the next hop groups and desync the next hop
 *              from those that contain it.
 *
 * Params:      IN  nh_key - The next hop to invalidate.
 *
 * Returns:     true, if the next hop was successfully invalidatedd from all
 *              containing groups;
 *              false, otherwise.
 */
bool NhgHandler::invalidateNextHop(const NextHopKey& nh_key)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_INFO("Invalidating next hop %s", nh_key.to_string().c_str());

    /*
     * Iterate through all groups and invalidate the next hop from those who
     * contain it.
     */
    for (auto& it : m_syncedNhgs)
    {
        auto& nhg = it.second.nhg;

        SWSS_LOG_INFO("Check if next hop is in group %s",
                        it.first.c_str());

        if (nhg.hasMember(nh_key))
        {
            SWSS_LOG_INFO("Group has next hop");

            /* If the desync fails, exit right away. */
            if (!nhg.invalidateNextHop(nh_key))
            {
                SWSS_LOG_WARN("Failed to invalidate next hop %s from group %s",
                                nh_key.to_string().c_str(),
                                it.first.c_str());
                return false;
            }
        }
    }

    return true;
}

/*
 * Purpose:     Get the next hop ID of the member.
 *
 * Description: Get the SAI ID of the next hop from NeighOrch.
 *
 * Params:      None.
 *
 * Returns:     The SAI ID of the next hop, or SAI_NULL_OBJECT_ID if the next
 *              hop is not valid.
 */
sai_object_id_t WeightedNhgMember::getNhId() const
{
    SWSS_LOG_ENTER();

    sai_object_id_t nh_id = SAI_NULL_OBJECT_ID;

    if (gNeighOrch->hasNextHop(m_key))
    {
        SWSS_LOG_INFO("NeighOrch has next hop %s",
                        m_key.to_string().c_str());
        nh_id = gNeighOrch->getNextHopId(m_key);
    }
    /*
     * If the next hop is labeled and the IP next hop exists, create the
     * labeled one over NeighOrch as it doesn't know about these next hops.
     * We don't do this in the constructor as the IP next hop may be added
     * after the object is created and would never create the labeled next hop
     * afterwards.
     */
    else if (isLabeled() && gNeighOrch->hasNextHop(m_key.ipKey()))
    {
        SWSS_LOG_INFO("Create labeled next hop %s",
                        m_key.to_string().c_str());
        gNeighOrch->addNextHop(m_key);
        nh_id = gNeighOrch->getNextHopId(m_key);
    }

    return nh_id;
}

/*
 * Purpose:     Move assignment operator.
 *
 * Description: Perform member-wise swap.
 *
 * Params:      IN  nhgm - The next hop group member to swap.
 *
 * Returns:     Reference to this object.
 */
WeightedNhgMember& WeightedNhgMember::operator=(WeightedNhgMember&& nhgm)
{
    SWSS_LOG_ENTER();

    m_weight = nhgm.m_weight;

    NhgMember::operator=(std::move(nhgm));

    return *this;
}

/*
 * Purpose:     Update the weight of a member.
 *
 * Description: Set the new member's weight and if the member is synced, update
 *              the SAI attribute as well.
 *
 * Params:      IN  weight - The weight of the next hop group member.
 *
 * Returns:     true, if the operation was successful;
 *              false, otherwise.
 */
bool WeightedNhgMember::updateWeight(uint8_t weight)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_INFO("Update group member %s weight from %u to %u",
                    m_key.to_string().c_str(),
                    m_weight,
                    weight);

    bool success = true;

    m_weight = weight;

    if (isSynced())
    {
        SWSS_LOG_INFO("Updating SAI weight attribute");

        sai_attribute_t nhgm_attr;
        nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT;
        nhgm_attr.value.s32 = m_weight;

        sai_status_t status = sai_next_hop_group_api->
                set_next_hop_group_member_attribute(m_id, &nhgm_attr);
        success = status == SAI_STATUS_SUCCESS;
    }

    SWSS_LOG_INFO("Returning %d", success);
    return success;
}

/*
 * Purpose:     Sync the group member with the given group member ID.
 *
 * Description: Set the group member's SAI ID to the the one given and
 *              increment the appropriate ref counters.
 *
 * Params:      IN  gm_id - The group member SAI ID to set.
 *
 * Returns:     Nothing.
 */
void WeightedNhgMember::sync(sai_object_id_t gm_id)
{
    SWSS_LOG_ENTER();

    NhgMember::sync(gm_id);
    gNeighOrch->increaseNextHopRefCount(m_key);
}

/*
 * Purpose:     Desync the group member, resetting it's SAI ID.
 *
 * Description: Reset the group member's SAI ID and decrement the appropriate
 *              ref counters.
 *
 * Params:      None.
 *
 * Returns:     Nothing.
 */
void WeightedNhgMember::desync()
{
    SWSS_LOG_ENTER();

    NhgMember::desync();
    gNeighOrch->decreaseNextHopRefCount(m_key);
}

/*
 * Purpose:     Destructor.
 *
 * Description: Assert the group member is desynced and remove the labeled
 *              next hop from NeighOrch if it is unreferenced.
 *
 * Params:      None.
 *
 * Returns:     Nothing.
 */
WeightedNhgMember::~WeightedNhgMember()
{
    SWSS_LOG_ENTER();

    /*
     * If the labeled next hop is unreferenced, delete it from NeighOrch as
     * NhgOrch and RouteOrch are the ones controlling it's lifetime.  They both
     * watch over these labeled next hops, so it doesn't matter who created
     * them as they're both doing the same checks before deleting a labeled
     * next hop.
     */
    if (isLabeled() &&
        gNeighOrch->hasNextHop(m_key) &&
        (gNeighOrch->getNextHopRefCount(m_key) == 0))
    {
        SWSS_LOG_INFO("Delete labeled next hop %s", m_key.to_string().c_str());
        gNeighOrch->removeNextHop(m_key);
    }
}

/*
 * Purpose:     Constructor.
 *
 * Description: Initialize the group's members based on the next hop group key.
 *
 * Params:      IN  key - The next hop group's key.
 *
 * Returns:     Nothing.
 */
Nhg::Nhg(const NextHopGroupKey& key) : NhgCommon(key), m_is_temp(false)
{
    SWSS_LOG_ENTER();

    /* Parse the key and create the members. */
    for (const auto &it : m_key.getNhsWithWts())
    {
        m_members.emplace(it.first, WeightedNhgMember(it));
    }
}

/*
 * Purpose:     Move assignment operator.
 *
 * Description: Perform member-wise swap.
 *
 * Params:      IN  nhg - The rvalue object to swap with.
 *
 * Returns:     Referene to this object.
 */
Nhg& Nhg::operator=(Nhg&& nhg)
{
    SWSS_LOG_ENTER();

    m_is_temp = nhg.m_is_temp;

    NhgCommon::operator=(std::move(nhg));

    return *this;
}

/*
 * Purpose:     Sync a next hop group.
 *
 * Description: Fill in the NHG ID.  If the group contains only one NH, this ID
 *              will be the SAI ID of the next hop that NeighOrch owns.  If it
 *              has more than one NH, create a group over the SAI API and then
 *              add it's members.
 *
 * Params:      None.
 *
 * Returns:     true, if the operation was successful;
 *              false, otherwise.
 */
bool Nhg::sync()
{
    SWSS_LOG_ENTER();

    /* If the group is already synced, exit. */
    if (isSynced())
    {
        SWSS_LOG_INFO("Group %s is already synced", to_string().c_str());
        return true;
    }

    /*
     * If the group has only one member, the group ID will be the member's NH
     * ID.
     */
    if (m_members.size() == 1)
    {
        const WeightedNhgMember& nhgm = m_members.begin()->second;

        if (nhgm.getNhId() == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_WARN("Next hop %s is not synced",
                            nhgm.getKey().to_string().c_str());
            return false;
        }
        else
        {
            m_id = nhgm.getNhId();
        }
    }
    /* If the key contains more than one NH, create a group. */
    else
    {
        /* Assert the group is not empty. */
        assert(!m_members.empty());

        /* Create the group over SAI. */
        sai_attribute_t nhg_attr;
        vector<sai_attribute_t> nhg_attrs;

        nhg_attr.id = SAI_NEXT_HOP_GROUP_ATTR_TYPE;
        nhg_attr.value.s32 = SAI_NEXT_HOP_GROUP_TYPE_ECMP;
        nhg_attrs.push_back(nhg_attr);

        sai_status_t status = sai_next_hop_group_api->create_next_hop_group(
                                                    &m_id,
                                                    gSwitchId,
                                                    (uint32_t)nhg_attrs.size(),
                                                    nhg_attrs.data());

        /* If the operation fails, exit. */
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create next hop group %s, rv:%d",
                            m_key.to_string().c_str(), status);
            return false;
        }

        SWSS_LOG_INFO("Next hop group %s has SAI ID %lu",
                        m_key.to_string().c_str(),
                        m_id);

        /* Increment the amount of programmed next hop groups. */
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);

        /* Increment the number of synced NHGs. */
        ++m_syncedCount;

        /*
        * Try creating the next hop group's members over SAI.
        */
        if (!syncMembers(m_key.getNextHops()))
        {
            SWSS_LOG_WARN("Failed to create next hop members of group %s",
                            to_string().c_str());
            return false;
        }
    }

    return true;
}

/*
 * Purpose:     Create a temporary next hop group when resources are exhausted.
 *
 * Description: Choose one member to represent the group and create a group
 *              with only that next hop as a member.  Any object referencing
 *              the SAI ID of a temporary group should keep querying NhgOrch in
 *              case the group is updated, as it's SAI ID will change at that
 *              point.
 *
 * Params:      IN  index   - The CP index of the next hop group.
 *
 * Returns:     The created temporary next hop group.
 */
Nhg NhgHandler::createTempNhg(const NextHopGroupKey& nhg_key)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_INFO("Syncing temporary group %s", nhg_key.to_string().c_str());

    /* Get a list of all valid next hops in the group. */
    std::list<NextHopKey> valid_nhs;

    for (const auto& nh_key : nhg_key.getNextHops())
    {
        /*
         * Check if the IP next hop exists.  We check for the IP next hop as
         * the group might contain labeled NHs which we should create if their
         * IP next hop does exist.
         */
        if (gNeighOrch->hasNextHop(nh_key.ipKey()))
        {
            SWSS_LOG_DEBUG("Next hop %s is a candidate for temporary group %s",
                            nh_key.to_string().c_str(),
                            nhg_key.to_string().c_str());
            valid_nhs.push_back(nh_key);
        }
    }

    /* If there is no valid member, exit. */
    if (valid_nhs.empty())
    {
        SWSS_LOG_INFO("There is no valid NH to sync temporary group %s",
                        nhg_key.to_string().c_str());
        throw std::logic_error("No valid NH in the key");
    }

    /* Randomly select the valid NH to represent the group. */
    auto it = valid_nhs.begin();
    advance(it, rand() % valid_nhs.size());

    /* Create the temporary group. */
    SWSS_LOG_INFO("Using next hop %s for the temporary NHG",
                    it->to_string().c_str());
    Nhg nhg(NextHopGroupKey(it->to_string()));
    nhg.setTemp(true);

    return nhg;
}

/*
 * Purpose:     Desync the next hop group.
 *
 * Description: Reset the group's SAI ID.  If the group has more than one
 *              members, desync the members and the group.
 *
 * Params:      None.
 *
 * Returns:     true, if the operation was successful;
 *              false, otherwise
 */
bool Nhg::desync()
{
    SWSS_LOG_ENTER();
    SWSS_LOG_INFO("Desyncing non CBF group %s", to_string().c_str());

    /*
     * If the group has just one member, simply reset it's SAI ID.
     */
    if (m_members.size() == 1)
    {
        SWSS_LOG_INFO("Group has just one member");
        m_id = SAI_NULL_OBJECT_ID;
        return true;
    }

    return NhgCommon::desync();
}

/*
 * Purpose:     Sync the given next hop group's members over the SAI API.
 *
 * Description: Iterate over the given members and sync them.  If the member
 *              is already synced, we skip it.  If any of the next hops isn't
 *              already synced by the neighOrch, this will fail.  Any next hop
 *              which has the neighbor interface down will be skipped.
 *
 * Params:      IN  nh_keys - The next hop keys of the members to sync.
 *
 * Returns:     true, if the members were added succesfully;
 *              false, otherwise.
 */
bool Nhg::syncMembers(const std::set<NextHopKey>& nh_keys)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_INFO("Adding next hop group %s members",
                    to_string().c_str());

    /* This method should not be called for groups with only one NH. */
    assert(m_members.size() > 1);

    ObjectBulker<sai_next_hop_group_api_t> nextHopGroupMemberBulker(
                                            sai_next_hop_group_api, gSwitchId);

    /*
     * Iterate over the given next hops.
     * If the group member is already synced, skip it.
     * If any next hop is not synced, thus neighOrch doesn't have it, stop
     * immediately.
     * If a next hop's interface is down, skip it from being synced.
     */
    std::map<NextHopKey, sai_object_id_t> syncingMembers;

    for (const auto& nh_key : nh_keys)
    {
        SWSS_LOG_INFO("Checking if next hop %s is valid",
                        nh_key.to_string().c_str());

        WeightedNhgMember& nhgm = m_members.at(nh_key);

        /* If the member is already synced, continue. */
        if (nhgm.isSynced())
        {
            SWSS_LOG_INFO("Update member");
            continue;
        }

        /*
         * If the next hop doesn't exist, stop from syncing the members.
         */
        if (nhgm.getNhId() == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_WARN("Failed to get next hop %s in group %s",
                        nhgm.to_string().c_str(), to_string().c_str());
            return false;
        }

        /* If the neighbor's interface is down, skip from being syncd. */
        if (gNeighOrch->isNextHopFlagSet(nh_key, NHFLAGS_IFDOWN))
        {
            SWSS_LOG_WARN("Skip next hop %s in group %s, interface is down",
                        nh_key.to_string().c_str(), to_string().c_str());
            continue;
        }

        /* Create the next hop group member's attributes and fill them. */
        vector<sai_attribute_t> nhgm_attrs = createNhgmAttrs(nhgm);

        /* Add a bulker entry for this member. */
        nextHopGroupMemberBulker.create_entry(&syncingMembers[nh_key],
                                               (uint32_t)nhgm_attrs.size(),
                                               nhgm_attrs.data());
    }

    /* Flush the bulker to perform the sync. */
    nextHopGroupMemberBulker.flush();

    /*
     * Go through the synced members and increment the Crm ref count for the
     * successful ones.
     */
    bool success = true;
    for (const auto& mbr : syncingMembers)
    {
        SWSS_LOG_INFO("Checking next hop member %s has a valid SAI ID",
                        mbr.first.to_string().c_str());

        /* Check that the returned member ID is valid. */
        if (mbr.second == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_ERROR("Failed to create next hop group %lu's member %s",
                            m_id, mbr.first.to_string().c_str());
            success = false;
        }
        else
        {
            m_members.at(mbr.first).sync(mbr.second);
        }
    }

    SWSS_LOG_INFO("Returning %d", success);

    return success;
}

/*
 * Purpose:     Update the next hop group based on a new next hop group key.
 *
 * Description: Update the group's members by removing the members that aren't
 *              in the new next hop group and adding the new members.  We first
 *              remove the missing members to avoid cases where we reached the
 *              ASIC group members limit.  This will not update the group's SAI
 *              ID in any way, unless we are promoting a temporary group.
 *
 * Params:      IN  nhg_key - The new next hop group key to update to.
 *
 * Returns:     true, if the operation was successful;
 *              false, otherwise.
 */
bool Nhg::update(const NextHopGroupKey& nhg_key)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_INFO("Update group %s with %s",
                    to_string().c_str(), nhg_key.to_string().c_str());

    /*
     * There are three cases where the SAI ID of the NHG will change:
     *  - changing a single next hop group to another single next hop group
     *  - changing a single next hop group to a multiple next hop group
     *  - changing a multiple next hop group to a single next hop group
     *
     * For these kind of updates, we can simply swap the existing group with
     * the updated group, as we have no way of preserving the existing SAI ID.
     *
     * Also, we can perform the same operation if the group is not synced at
     * all.
     */
    if ((nhg_key.getSize() == 1) || (m_members.size() == 1) || !isSynced())
    {
        SWSS_LOG_INFO("Updating group without preserving it's SAI ID");

        bool was_synced = isSynced();
        *this = Nhg(nhg_key);

        /* Sync the group only if it was synced before. */
        return (was_synced ? sync() : true);
    }
    /*
     * If we are updating a multiple next hop group to another multiple next
     * hop group, we can preserve it's SAI ID by only updating it's members.
     * This way, any objects referencing the SAI ID of this object will
     * continue to work.
     */
    else
    {
        /* Update the key. */
        m_key = nhg_key;

        std::map<NextHopKey, uint8_t> new_nhgms = nhg_key.getNhsWithWts();
        std::set<NextHopKey> removed_nh_keys;

        /* Mark the members that need to be removed. */
        for (auto& mbr_it : m_members)
        {
            const NextHopKey& nh_key = mbr_it.first;

            /* Look for the existing member inside the new ones. */
            const auto& new_nhgm_it = new_nhgms.find(nh_key);

            /* If the member is not found, then it needs to be removed. */
            if (new_nhgm_it == new_nhgms.end())
            {
                SWSS_LOG_INFO("Add member %s to be desynced",
                                nh_key.to_string().c_str());
                removed_nh_keys.insert(nh_key);
            }
            /* If the member is updated, update it's weight. */
            else
            {
                if (!mbr_it.second.updateWeight(new_nhgm_it->second))
                {
                    SWSS_LOG_WARN("Failed to update member %s weight",
                                nh_key.to_string().c_str());
                    return false;
                }

                /*
                 * Erase the member from the new members list as it already
                 * exists.
                 */
                new_nhgms.erase(new_nhgm_it);
            }
        }

        /* Desync the removed members. */
        if (!desyncMembers(removed_nh_keys))
        {
            SWSS_LOG_WARN("Failed to desync members from group %s",
                            to_string().c_str());
            return false;
        }

        /* Remove the desynced members. */
        for (const auto& nh_key : removed_nh_keys)
        {
            m_members.erase(nh_key);
        }

        /* Add any new members to the group. */
        for (const auto& it : new_nhgms)
        {
            m_members.emplace(it.first, WeightedNhgMember(it));
        }

        /*
         * Sync all the members of the group.  We sync all of them because
         * there may be previous members that were not successfully synced
         * before the update, so we must make sure we sync those as well.
         */
        if (!syncMembers(m_key.getNextHops()))
        {
            SWSS_LOG_WARN("Failed to sync new members for group %s",
                            to_string().c_str());
            return false;
        }

        return true;
    }
}

/*
 * Purpose:     Create the attributes vector for a next hop group member.
 *
 * Description: Create the group ID, next hop ID and weight attributes.
 *
 * Params:      IN  nhgm - The next hop group member.
 *
 * Returns:     The attributes vector for the given next hop.
 */
vector<sai_attribute_t> Nhg::createNhgmAttrs(
                                        const WeightedNhgMember& nhgm) const
{
    SWSS_LOG_ENTER();

    vector<sai_attribute_t> nhgm_attrs;
    sai_attribute_t nhgm_attr;

    /* Fill in the group ID. */
    nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
    nhgm_attr.value.oid = m_id;
    nhgm_attrs.push_back(nhgm_attr);

    /* Fill in the next hop ID. */
    nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
    nhgm_attr.value.oid = nhgm.getNhId();
    nhgm_attrs.push_back(nhgm_attr);

    nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT;
    nhgm_attr.value.s32 = nhgm.getWeight();
    nhgm_attrs.push_back(nhgm_attr);

    return nhgm_attrs;
}

/*
 * Purpose:     Validate a next hop in the group.
 *
 * Description: Sync the validated next hop group member.
 *
 * Params:      IN  nh_key - The next hop to validate.
 *
 * Returns:     true, if the operation was successful;
 *              false, otherwise.
 */
bool Nhg::validateNextHop(const NextHopKey& nh_key)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_INFO("Validate NH %s in group %s",
                    nh_key.to_string().c_str(),
                    to_string().c_str());

    /*
     * If the group has only one member, there is nothing to be done.  The
     * member is only a reference to the next hop owned by NeighOrch, so it is
     * not for us to take any decisions regarding those.
     */
    if (m_members.size() == 1)
    {
        return true;
    }

    return syncMembers({nh_key});
}

/*
 * Purpose:     Invalidate a next hop in the group.
 *
 * Description: Sync the invalidated next hop group member.
 *
 * Params:      IN  nh_key - The next hop to invalidate.
 *
 * Returns:     true, if the operation was successful;
 *              false, otherwise.
 */
bool Nhg::invalidateNextHop(const NextHopKey& nh_key)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_INFO("Invalidate NH %s in group %s",
                    nh_key.to_string().c_str(),
                    to_string().c_str());

    /*
     * If the group has only one member, there is nothing to be done.  The
     * member is only a reference to the next hop owned by NeighOrch, so it is
     * not for us to take any decisions regarding those.
     */
    if (m_members.size() == 1)
    {
        return true;
    }

    return desyncMembers({nh_key});
}

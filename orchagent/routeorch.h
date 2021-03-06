#ifndef SWSS_ROUTEORCH_H
#define SWSS_ROUTEORCH_H

#include "orch.h"
#include "observer.h"
#include "switchorch.h"
#include "intfsorch.h"
#include "neighorch.h"
#include "vxlanorch.h"

#include "ipaddress.h"
#include "ipaddresses.h"
#include "ipprefix.h"
#include "nexthopgroupkey.h"
#include "bulker.h"
#include "fgnhgorch.h"
#include <map>

/* Maximum next hop group number */
#define NHGRP_MAX_SIZE 128
/* Length of the Interface Id value in EUI64 format */
#define EUI64_INTF_ID_LEN 8

#define LOOPBACK_PREFIX     "Loopback"

typedef std::map<NextHopKey, sai_object_id_t> NextHopGroupMembers;

struct NextHopGroupEntry
{
    sai_object_id_t         next_hop_group_id;      // next hop group id
    int                     ref_count;              // reference count
    NextHopGroupMembers     nhopgroup_members;      // ids of members indexed by <ip_address, if_alias>
};

struct NextHopUpdate
{
    sai_object_id_t vrf_id;
    IpAddress destination;
    IpPrefix prefix;
    NextHopGroupKey nexthopGroup;
};

/*
 * Structure describing the next hop group used by a route.  As the next hop
 * groups can either be owned by RouteOrch or by NhgOrch, we have to keep track
 * of the next hop group index, as it is the one telling us which one owns it.
 */
struct RouteNhg
{
    NextHopGroupKey nhg_key;

    /*
     * Index of the next hop group used.  Filled only if referencing a
     * NhgOrch's owned next hop group.
     */
    std::string nhg_index;

    RouteNhg() = default;
    RouteNhg(const NextHopGroupKey& key, const std::string& index) :
        nhg_key(key), nhg_index(index) {}

    bool operator==(const RouteNhg& rnhg)
       { return ((nhg_key == rnhg.nhg_key) && (nhg_index == rnhg.nhg_index)); }
    bool operator!=(const RouteNhg& rnhg) { return !(*this == rnhg); }
};

struct NextHopObserverEntry;

/* NextHopGroupTable: NextHopGroupKey, NextHopGroupEntry */
typedef std::map<NextHopGroupKey, NextHopGroupEntry> NextHopGroupTable;
/* RouteTable: destination network, NextHopGroupKey */
typedef std::map<IpPrefix, RouteNhg> RouteTable;
/* RouteTables: vrf_id, RouteTable */
typedef std::map<sai_object_id_t, RouteTable> RouteTables;
/* Host: vrf_id, IpAddress */
typedef std::pair<sai_object_id_t, IpAddress> Host;
/* NextHopObserverTable: Host, next hop observer entry */
typedef std::map<Host, NextHopObserverEntry> NextHopObserverTable;
/* LabelRouteTable: destination label, next hop address(es) */
typedef std::map<Label, RouteNhg> LabelRouteTable;
/* LabelRouteTables: vrf_id, LabelRouteTable */
typedef std::map<sai_object_id_t, LabelRouteTable> LabelRouteTables;

struct NextHopObserverEntry
{
    RouteTable routeTable;
    list<Observer *> observers;
};

struct RouteBulkContext
{
    std::deque<sai_status_t>            object_statuses;    // Bulk statuses
    NextHopGroupKey                     tmp_next_hop;       // Temporary next hop
    NextHopGroupKey                     nhg;
    std::string                         nhg_index;
    sai_object_id_t                     vrf_id;
    IpPrefix                            ip_prefix;
    bool                                excp_intfs_flag;
    std::vector<string>                 ipv;
    // is_temp will track if the NhgOrch's owned NHG is temporary or not
    bool                                is_temp;

    RouteBulkContext()
        : excp_intfs_flag(false), is_temp(false)
    {
    }

    // Disable any copy constructors
    RouteBulkContext(const RouteBulkContext&) = delete;
    RouteBulkContext(RouteBulkContext&&) = delete;

    void clear()
    {
        object_statuses.clear();
        tmp_next_hop.clear();
        nhg.clear();
        ipv.clear();
        excp_intfs_flag = false;
        vrf_id = SAI_NULL_OBJECT_ID;
        is_temp = false;
    }
};

struct LabelRouteBulkContext
{
    std::deque<sai_status_t>            object_statuses;    // Bulk statuses
    NextHopGroupKey                     tmp_next_hop;       // Temporary next hop
    NextHopGroupKey                     nhg;
    std::string                         nhg_index;
    sai_object_id_t                     vrf_id;
    Label                               label;
    bool                                excp_intfs_flag;
    std::vector<string>                 ipv;
    // is_temp will track if the NhgOrch's owned NHG is temporary or not
    bool                                is_temp;

    LabelRouteBulkContext()
        : excp_intfs_flag(false), is_temp(false)
    {
    }

    // Disable any copy constructors
    LabelRouteBulkContext(const LabelRouteBulkContext&) = delete;
    LabelRouteBulkContext(LabelRouteBulkContext&&) = delete;

    void clear()
    {
        object_statuses.clear();
        tmp_next_hop.clear();
        nhg.clear();
        ipv.clear();
        excp_intfs_flag = false;
        vrf_id = SAI_NULL_OBJECT_ID;
    }
};

class RouteOrch : public Orch, public Subject
{
public:
    RouteOrch(DBConnector *db, vector<table_name_with_pri_t> &tableNames, NeighOrch *neighOrch, IntfsOrch *intfsOrch, VRFOrch *vrfOrch, FgNhgOrch *fgNhgOrch);

    bool hasNextHopGroup(const NextHopGroupKey&) const;
    sai_object_id_t getNextHopGroupId(const NextHopGroupKey&);

    void attach(Observer *, const IpAddress&, sai_object_id_t vrf_id = gVirtualRouterId);
    void detach(Observer *, const IpAddress&, sai_object_id_t vrf_id = gVirtualRouterId);

    void increaseNextHopRefCount(const NextHopGroupKey&);
    void decreaseNextHopRefCount(const NextHopGroupKey&);
    bool isRefCounterZero(const NextHopGroupKey&) const;

    bool addNextHopGroup(const NextHopGroupKey&);
    bool removeNextHopGroup(const NextHopGroupKey&);

    bool validnexthopinNextHopGroup(const NextHopKey&);
    bool invalidnexthopinNextHopGroup(const NextHopKey&);

    bool createRemoteVtep(sai_object_id_t, const NextHopKey&);
    bool deleteRemoteVtep(sai_object_id_t, const NextHopKey&);
    bool removeOverlayNextHops(sai_object_id_t, const NextHopGroupKey&);

    void notifyNextHopChangeObservers(sai_object_id_t, const IpPrefix&, const NextHopGroupKey&, bool);
    const NextHopGroupKey getSyncdRouteNhgKey(sai_object_id_t vrf_id, const IpPrefix& ipPrefix);
    bool createFineGrainedNextHopGroup(sai_object_id_t &next_hop_group_id, vector<sai_attribute_t> &nhg_attrs);
    bool removeFineGrainedNextHopGroup(sai_object_id_t &next_hop_group_id);

private:
    NeighOrch *m_neighOrch;
    IntfsOrch *m_intfsOrch;
    VRFOrch *m_vrfOrch;
    FgNhgOrch *m_fgNhgOrch;

    bool m_resync;

    RouteTables m_syncdRoutes;
    LabelRouteTables m_syncdLabelRoutes;
    NextHopGroupTable m_syncdNextHopGroups;

    std::set<NextHopGroupKey> m_bulkNhgReducedRefCnt;

    NextHopObserverTable m_nextHopObservers;

    EntityBulker<sai_route_api_t>           gRouteBulker;
    EntityBulker<sai_mpls_api_t>            gLabelRouteBulker;
    ObjectBulker<sai_next_hop_group_api_t>  gNextHopGroupMemberBulker;

    void addTempRoute(RouteBulkContext& ctx, const NextHopGroupKey&);
    bool addRoute(RouteBulkContext& ctx, const NextHopGroupKey&);
    bool removeRoute(RouteBulkContext& ctx);
    bool addRoutePost(const RouteBulkContext& ctx, const NextHopGroupKey &nextHops);
    bool removeRoutePost(const RouteBulkContext& ctx);

    void addTempLabelRoute(LabelRouteBulkContext& ctx, const NextHopGroupKey&);
    bool addLabelRoute(LabelRouteBulkContext& ctx, const NextHopGroupKey&);
    bool removeLabelRoute(LabelRouteBulkContext& ctx);
    bool addLabelRoutePost(const LabelRouteBulkContext& ctx, const NextHopGroupKey &nextHops);
    bool removeLabelRoutePost(const LabelRouteBulkContext& ctx);

    std::string getLinkLocalEui64Addr(void);
    void        addLinkLocalRouteToMe(sai_object_id_t vrf_id, IpPrefix linklocal_prefix);

    void doLabelTask(Consumer& consumer);
    void doPrefixTask(Consumer& consumer);
    void doTask(Consumer& consumer);
};

#endif /* SWSS_ROUTEORCH_H */

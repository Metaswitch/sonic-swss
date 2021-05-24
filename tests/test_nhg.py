import os
import re
import sys
import time
import json
import pytest
import ipaddress

from swsscommon import swsscommon as swss


class TestNextHopGroup(object):
    ASIC_NHS_STR = "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP"
    ASIC_NHG_STR = "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP"
    ASIC_NHGM_STR = ASIC_NHG_STR + "_MEMBER"
    ASIC_RT_STR = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY"
    ASIC_INSEG_STR = "ASIC_STATE:SAI_OBJECT_TYPE_INSEG_ENTRY"

    def get_route_id(self, prefix, dvs):
        for k in dvs.get_asic_db().get_keys(self.ASIC_RT_STR):
            if json.loads(k)['dest'] == prefix:
                return k

        return None

    def get_inseg_id(self, label, dvs):
        for k in dvs.get_asic_db().get_keys(self.ASIC_INSEG_STR):
            print(json.loads(k))
            if json.loads(k)['label'] == label:
                return k

        return None

    def get_nhg_id(self, nhg_index, dvs):
        # Add a route with the given index, then retrieve the next hop group ID
        # from that route
        asic_db = dvs.get_asic_db()
        asic_rts_count = len(asic_db.get_keys(self.ASIC_RT_STR))

        fvs = swss.FieldValuePairs([('nexthop_group', nhg_index)])
        prefix = '255.255.255.255/24'
        ps = swss.ProducerStateTable(dvs.get_app_db().db_connection,
                                            "ROUTE_TABLE")
        ps.set(prefix, fvs)

        # Assert the route is created
        try:
            asic_db.wait_for_n_keys(self.ASIC_RT_STR, asic_rts_count + 1)
        except:
            return None

        # Get the route ID for the created route
        rt_id = self.get_route_id(prefix, dvs)
        assert rt_id != None

        # Get the NHGID
        nhgid = asic_db.get_entry(self.ASIC_RT_STR, rt_id)["SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID"]

        # Remove the added route
        ps._del(prefix)
        asic_db.wait_for_deleted_entry(self.ASIC_RT_STR, rt_id)

        # Return the NHGID
        return nhgid

    def get_nhgm_ids(self, nhg_index, dvs, nh_id=None):
        if nh_id is None:
            nhgid = self.get_nhg_id(nhg_index, dvs)
        else:
            nhgid = nh_id
        nhgms = []
        asic_db = dvs.get_asic_db()

        for k in asic_db.get_keys(self.ASIC_NHGM_STR):
            fvs = asic_db.get_entry(self.ASIC_NHGM_STR, k)

            # Sometimes some of the NHGMs have no fvs for some reason, so
            # we skip those
            try:
                if fvs['SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID'] == nhgid:
                    nhgms.append(k)
            except KeyError as e:
                pass

        return nhgms

    def port_name(self, i):
        return "Ethernet" + str(i * 4)

    def port_ip(self, i):
        return "10.0.0." + str(i * 2)

    def port_ipprefix(self, i):
        return self.port_ip(i) + "/31"

    def peer_ip(self, i):
        return "10.0.0." + str(i * 2 + 1)

    def port_mac(self, i):
        return "00:00:00:00:00:0" + str(i)

    def config_intf(self, i, dvs):
        config_db = dvs.get_config_db()
        fvs = {'NULL': 'NULL'}

        config_db.create_entry("INTERFACE", self.port_name(i), fvs)
        config_db.create_entry("INTERFACE", "{}|{}".format(self.port_name(i), self.port_ipprefix(i)), fvs)
        dvs.runcmd("config interface startup " + self.port_name(i))
        dvs.runcmd("arp -s {} {}".format(self.peer_ip(i), self.port_mac(i)))
        assert dvs.servers[i].runcmd("ip link set down dev eth0") == 0
        assert dvs.servers[i].runcmd("ip link set up dev eth0") == 0

    def flap_intf(self, i, status, dvs):
        assert status in ['up', 'down']

        dvs.servers[i].runcmd("ip link set {} dev eth0".format(status)) == 0
        time.sleep(1)
        fvs = dvs.get_app_db().get_entry("PORT_TABLE", "Ethernet%d" % (i * 4))
        assert bool(fvs)
        assert fvs["oper_status"] == status

    def test_route_nhg(self, dvs, testlog):
        for i in range(3):
            self.config_intf(i, dvs)

        app_db = dvs.get_app_db()
        ps = swss.ProducerStateTable(app_db.db_connection, "ROUTE_TABLE")

        asic_db = dvs.get_asic_db()
        asic_routes_count = len(asic_db.get_keys(self.ASIC_RT_STR))

        fvs = swss.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.3,10.0.0.5"),
                                            ("ifname", "Ethernet0,Ethernet4,Ethernet8")])
        ps.set("2.2.2.0/24", fvs)

        # check if route was propagated to ASIC DB

        asic_db.wait_for_n_keys(self.ASIC_RT_STR, asic_routes_count + 1)
        keys = asic_db.get_keys(self.ASIC_RT_STR)

        k = self.get_route_id('2.2.2.0/24', dvs)
        assert k is not None

        # assert the route points to next hop group
        fvs = asic_db.get_entry(self.ASIC_RT_STR, k)

        nhgid = fvs["SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID"]

        fvs = asic_db.get_entry(self.ASIC_NHG_STR, nhgid)

        assert bool(fvs)

        keys = asic_db.get_keys(self.ASIC_NHGM_STR)

        assert len(keys) == 3

        for k in keys:
            fvs = asic_db.get_entry(self.ASIC_NHGM_STR, k)

            assert fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"] == nhgid

        # bring links down one-by-one
        for i in [0, 1, 2]:
            self.flap_intf(i, 'down', dvs)

            keys = asic_db.get_keys(self.ASIC_NHGM_STR)

            assert len(keys) == 2 - i

        # bring links up one-by-one
        for i in [0, 1, 2]:
            self.flap_intf(i, 'up', dvs)

            keys = asic_db.get_keys(self.ASIC_NHGM_STR)

            assert len(keys) == i + 1

            for k in keys:
                fvs = asic_db.get_entry(self.ASIC_NHGM_STR, k)
                assert fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"] == nhgid

        # Remove route 2.2.2.0/24
        ps._del("2.2.2.0/24")

        # Wait for route 2.2.2.0/24 to be removed
        asic_db.wait_for_n_keys(self.ASIC_RT_STR, asic_routes_count)

    def test_label_route_nhg(self, dvs, testlog):
        for i in range(3):
            self.config_intf(i, dvs)

        app_db = dvs.get_app_db()
        lr_ps = swss.ProducerStateTable(app_db.db_connection, "LABEL_ROUTE_TABLE")

        asic_db = dvs.get_asic_db()
        asic_insegs_count = len(asic_db.get_keys(self.ASIC_INSEG_STR))

        # add label route
        fvs = swss.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.3,10.0.0.5"),
                                            ("ifname", "Ethernet0,Ethernet4,Ethernet8")])
        lr_ps.set("10", fvs)

        # check if route was propagated to ASIC DB

        asic_db.wait_for_n_keys(self.ASIC_INSEG_STR, asic_insegs_count + 1)
        keys = asic_db.get_keys(self.ASIC_INSEG_STR)

        k = self.get_inseg_id('10', dvs)
        assert k is not None

        # assert the route points to next hop group
        fvs = asic_db.get_entry(self.ASIC_INSEG_STR, k)

        nhgid = fvs["SAI_INSEG_ENTRY_ATTR_NEXT_HOP_ID"]

        fvs = asic_db.get_entry(self.ASIC_NHG_STR, nhgid)

        assert bool(fvs)

        keys = asic_db.get_keys(self.ASIC_NHGM_STR)

        assert len(keys) == 3

        for k in keys:
            fvs = asic_db.get_entry(self.ASIC_NHGM_STR, k)

            assert fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"] == nhgid

        # bring links down one-by-one
        for i in [0, 1, 2]:
            self.flap_intf(i, 'down', dvs)

            keys = asic_db.get_keys(self.ASIC_NHGM_STR)

            assert len(keys) == 2 - i

        # bring links up one-by-one
        for i in [0, 1, 2]:
            self.flap_intf(i, 'up', dvs)

            keys = asic_db.get_keys(self.ASIC_NHGM_STR)

            assert len(keys) == i + 1

            for k in keys:
                fvs = asic_db.get_entry(self.ASIC_NHGM_STR, k)
                assert fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"] == nhgid

        # Remove label route 10
        lr_ps._del("10")

        # Wait for label route 10 to be removed
        asic_db.wait_for_n_keys(self.ASIC_INSEG_STR, asic_insegs_count)

    def test_nhgorch_labeled_nhs(self, dvs, testlog):
        for i in range(2):
            self.config_intf(i, dvs)

        app_db = dvs.get_app_db()
        asic_db = dvs.get_asic_db()
        nhg_ps = swss.ProducerStateTable(app_db.db_connection, "NEXT_HOP_GROUP_TABLE")
        asic_nhgs_count = len(asic_db.get_keys(self.ASIC_NHG_STR))
        asic_nhgms_count = len(asic_db.get_keys(self.ASIC_NHGM_STR))
        asic_nhs_count = len(asic_db.get_keys(self.ASIC_NHS_STR))

        # Add a group containing labeled weighted NHs
        fvs = swss.FieldValuePairs([('nexthop', '1+10.0.0.1,3+10.0.0.3'),
                                            ('ifname', 'Ethernet0,Ethernet4'),
                                            ('weight', '2,4')])
        nhg_ps.set('group1', fvs)
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count + 1)
        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, asic_nhgms_count + 2)

        # NhgOrch should create two next hops for the labeled ones
        asic_db.wait_for_n_keys(self.ASIC_NHS_STR, asic_nhs_count + 2)

        # Assert the weights are properly set
        nhgm_ids = self.get_nhgm_ids('group1', dvs)
        weights = []
        for k in nhgm_ids:
            fvs = asic_db.get_entry(self.ASIC_NHGM_STR, k)
            weights.append(fvs['SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT'])
        assert set(weights) == set(['2', '4'])

        # Create a new single next hop with the same label
        fvs = swss.FieldValuePairs([('nexthop', '1+10.0.0.1'),
                                            ('ifname', 'Ethernet0')])
        nhg_ps.set('group2', fvs)

        # No new next hop should be added
        time.sleep(1)
        assert len(asic_db.get_keys(self.ASIC_NHS_STR)) == asic_nhs_count + 2

        # Create a new single next hop with a different label
        fvs = swss.FieldValuePairs([('nexthop', '2+10.0.0.1'),
                                            ('ifname', 'Ethernet0')])
        nhg_ps.set('group3', fvs)

        # A new next hop should be added
        asic_db.wait_for_n_keys(self.ASIC_NHS_STR, asic_nhs_count + 3)

        # Delete group3
        nhg_ps._del('group3')

        # Group3's NH should be deleted
        asic_db.wait_for_n_keys(self.ASIC_NHS_STR, asic_nhs_count + 2)

        # Delete group2
        nhg_ps._del('group2')

        # The number of NHs should be the same as they are still referenced by
        # group1
        time.sleep(1)
        asic_db.wait_for_n_keys(self.ASIC_NHS_STR, asic_nhs_count + 2)

        # Update group1 with one weight only
        fvs = swss.FieldValuePairs([('nexthop', '2+10.0.0.1,4+10.0.0.3'),
                                            ('ifname', 'Ethernet0,Ethernet4'),
                                            ('weight', '2')])
        nhg_ps.set('group1', fvs)

        # The next hop group members and next hops should be replaced
        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, asic_nhgms_count + 2)
        asic_db.wait_for_n_keys(self.ASIC_NHS_STR, asic_nhs_count + 2)

        # Assert the weights of the NHGMs are the expected ones
        nhgm_ids = self.get_nhgm_ids('group1', dvs)
        weights = set()
        for nhgm_id in nhgm_ids:
            fvs = asic_db.get_entry(self.ASIC_NHGM_STR, nhgm_id)
            weights.add(fvs['SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT'])
        assert weights == set(['2', '1'])

        # Update group1 with no weights and both labeled and unlabeled NHs
        fvs = swss.FieldValuePairs([('nexthop', '2+10.0.0.1,10.0.0.3'),
                                            ('ifname', 'Ethernet0,Ethernet4')])
        nhg_ps.set('group1', fvs)

        # Group members should be replaced and one NH should get deleted
        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, asic_nhgms_count + 2)
        asic_db.wait_for_n_keys(self.ASIC_NHS_STR, asic_nhs_count + 1)

        # Assert the weights of the NHGMs are the expected ones
        nhgm_ids = self.get_nhgm_ids('group1', dvs)
        weights = []
        for nhgm_id in nhgm_ids:
            fvs = asic_db.get_entry(self.ASIC_NHGM_STR, nhgm_id)
            weights.append(fvs['SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT'])
        assert weights == ['1', '1']

        # Delete group1
        nhg_ps._del('group1')

        # Wait for the group and it's members to be deleted
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count)
        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, asic_nhgms_count)

        # The two next hops should also get deleted
        asic_db.wait_for_n_keys(self.ASIC_NHS_STR, asic_nhs_count)

    def test_nhgorch_excp_group_cases(self, dvs, testlog):
        for i in range(3):
            self.config_intf(i, dvs)

        app_db = dvs.get_app_db()
        asic_db = dvs.get_asic_db()
        nhg_ps = swss.ProducerStateTable(app_db.db_connection, "NEXT_HOP_GROUP_TABLE")
        rt_ps = swss.ProducerStateTable(app_db.db_connection, "ROUTE_TABLE")

        def get_nhg_keys():
            return asic_db.get_keys(self.ASIC_NHG_STR)

        def get_nhgm_keys():
            return asic_db.get_keys(self.ASIC_NHGM_STR)

        def get_rt_keys():
            return asic_db.get_keys(self.ASIC_RT_STR)

        # Count existing objects
        prev_nhg_keys = get_nhg_keys()
        asic_nhgs_count = len(get_nhg_keys())
        asic_nhgms_count = len(get_nhgm_keys())
        asic_nhs_count = len(asic_db.get_keys(self.ASIC_NHS_STR))

        # Remove a group that does not exist
        nhg_ps._del("group1")
        time.sleep(1)
        assert len(get_nhg_keys()) == asic_nhgs_count

        # Create a next hop group with a member that does not exist - should fail
        fvs = swss.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3,10.0.0.63'),
                                        ("ifname", "Ethernet0,Ethernet4,Ethernet124")])
        nhg_ps.set("group1", fvs)
        time.sleep(1)
        assert len(asic_db.get_keys(self.ASIC_NHG_STR)) == asic_nhgs_count

        # Issue an update for this next hop group that doesn't yet exist,
        # which contains only valid NHs.  This will overwrite the previous
        # operation and create the group.
        fvs = swss.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.5'),
                                        ("ifname", "Ethernet0,Ethernet8")])
        nhg_ps.set("group1", fvs)
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count + 1)
        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, asic_nhgms_count + 2)

        # Check the group has it's two members
        for nhgid in asic_db.get_keys(self.ASIC_NHG_STR):
            if nhgid not in prev_nhg_keys:
                break

        count = 0
        for k in asic_db.get_keys(self.ASIC_NHGM_STR):
            fvs = asic_db.get_entry(self.ASIC_NHGM_STR, k)
            if fvs['SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID'] == nhgid:
                count += 1
        assert count == 2

        # Add a route referencing the new group
        asic_rts_count = len(get_rt_keys())
        fvs = swss.FieldValuePairs([('nexthop_group', 'group1')])
        rt_ps.set('2.2.2.0/24', fvs)
        asic_db.wait_for_n_keys(self.ASIC_RT_STR, asic_rts_count + 1)

        # Get the route key
        for rt_key in get_rt_keys():
            k = json.loads(rt_key)

            if k['dest'] == "2.2.2.0/24":
                break

        # Try removing the group while it still has references - should fail
        nhg_ps._del('group1')
        time.sleep(1)
        assert len(get_nhg_keys()) == asic_nhgs_count + 1

        # Create a new group
        fvs = swss.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3'),
                                          ('ifname', 'Ethernet0,Ethernet4')])
        nhg_ps.set("group2", fvs)
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count + 2)
        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, asic_nhgms_count + 4)

        # Update the route to point to the new group
        fvs = swss.FieldValuePairs([('nexthop_group', 'group2')])
        rt_ps.set('2.2.2.0/24', fvs)

        # The first group should have got deleted
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count + 1)
        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, asic_nhgms_count + 2)

        assert asic_db.get_entry(self.ASIC_RT_STR, rt_key)['SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID'] != nhgid

        # Update the route with routeOrch's owned next hop group
        nhgid = asic_db.get_entry(self.ASIC_RT_STR, rt_key)['SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID']
        fvs = swss.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3'),
                                          ('ifname', 'Ethernet0,Ethernet4')])
        rt_ps.set('2.2.2.0/24', fvs)

        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count + 2)
        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, asic_nhgms_count + 4)

        # Assert the next hop group ID changed
        assert asic_db.get_entry(self.ASIC_RT_STR, rt_key)['SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID'] != nhgid
        nhgid = asic_db.get_entry(self.ASIC_RT_STR, rt_key)['SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID']

        # Update the route to point back to group2
        fvs = swss.FieldValuePairs([('nexthop_group', 'group2')])
        rt_ps.set('2.2.2.0/24', fvs)

        # The routeOrch's owned next hop group should get deleted
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count + 1)
        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, asic_nhgms_count + 2)

        # Assert the route points back to group2
        assert asic_db.get_entry(self.ASIC_RT_STR, rt_key)['SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID'] != nhgid

        # Create a new group with the same members as group2
        nhgid = asic_db.get_entry(self.ASIC_RT_STR, rt_key)['SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID']
        fvs = swss.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3'),
                                          ('ifname', 'Ethernet0,Ethernet4')])
        nhg_ps.set("group1", fvs)
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count + 2)
        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, asic_nhgms_count + 4)

        # Update the route to point to the new group
        fvs = swss.FieldValuePairs([('nexthop_group', 'group1')])
        rt_ps.set('2.2.2.0/24', fvs)
        time.sleep(1)

        # Assert the next hop group ID changed
        assert asic_db.get_entry(self.ASIC_RT_STR, rt_key)['SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID'] != nhgid

        # Delete group1 while being referenced
        nhg_ps._del('group1')
        time.sleep(1)
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count + 2)

        # Update group1
        fvs = swss.FieldValuePairs([('nexthop', '10.0.0.3,10.0.0.5'),
                                          ('ifname', 'Ethernet4,Ethernet8')])
        nhg_ps.set("group1", fvs)
        time.sleep(1)

        # Remove the route
        rt_ps._del('2.2.2.0/24')
        asic_db.wait_for_n_keys(self.ASIC_RT_STR, asic_rts_count)

        # The group1 update should have overwritten the delete
        time.sleep(1)
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count + 2)

        # Remove the groups
        nhg_ps._del('group1')
        nhg_ps._del('group2')

        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count)
        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, asic_nhgms_count)

        # Create a route with labeled NHs
        fvs = swss.FieldValuePairs([('nexthop', '1+10.0.0.1,3+10.0.0.3'),
                                            ('ifname', 'Ethernet0,Ethernet4'),
                                            ('weight', '2,4')])
        rt_ps.set('2.2.2.0/24', fvs)
        asic_db.wait_for_n_keys(self.ASIC_RT_STR, asic_rts_count + 1)

        # Two new next hops should be created
        asic_db.wait_for_n_keys(self.ASIC_NHS_STR, asic_nhs_count + 2)

        # Create a NHG with the same details
        nhg_ps.set('group1', fvs)

        # No new next hops should be created
        time.sleep(1)
        assert len(asic_db.get_keys(self.ASIC_NHS_STR)) == asic_nhs_count + 2

        # Update the group with a different NH
        fvs = swss.FieldValuePairs([('nexthop', '2+10.0.0.1,3+10.0.0.3'),
                                            ('ifname', 'Ethernet0,Ethernet4'),
                                            ('weight', '2,4')])
        nhg_ps.set('group1', fvs)

        # A new next hop should be created
        asic_db.wait_for_n_keys(self.ASIC_NHS_STR, asic_nhs_count + 3)

        # Remove the route
        rt_ps._del('2.2.2.0/24')

        # One NH should become unreferenced and should be deleted.  The other
        # one is still referenced by NhgOrch's owned NHG.
        asic_db.wait_for_n_keys(self.ASIC_NHS_STR, asic_nhs_count + 2)

        # Remove the group
        nhg_ps._del('group1')

        # Both new next hops should be deleted
        asic_db.wait_for_n_keys(self.ASIC_NHS_STR, asic_nhs_count)

        # Add a route with a NHG that does not exist
        fvs = swss.FieldValuePairs([('nexthop_group', 'group1')])
        rt_ps.set('2.2.2.0/24', fvs)
        time.sleep(1)
        assert asic_rts_count == len(asic_db.get_keys(self.ASIC_RT_STR))

        # Remove the pending route
        rt_ps._del('2.2.2.0/24')

    def test_route_nhg_exhaust(self, dvs, testlog):
        """
        Test the situation of exhausting ECMP group, assume SAI_SWITCH_ATTR_NUMBER_OF_ECMP_GROUPS is 512

        In order to achieve that, we will config
            1. 9 ports
            2. 512 routes with different nexthop group

        See Also
        --------
        SwitchStateBase::set_number_of_ecmp_groups()
        https://github.com/Azure/sonic-sairedis/blob/master/vslib/src/SwitchStateBase.cpp

        """

        # TODO: check ECMP 512

        def gen_ipprefix(r):
            """ Construct route like 2.X.X.0/24 """
            ip = ipaddress.IPv4Address(IP_INTEGER_BASE + r * 256)
            ip = str(ip)
            ipprefix = ip + "/24"
            return ipprefix

        def gen_nhg_fvs(binary):
            nexthop = []
            ifname = []
            for i in range(MAX_PORT_COUNT):
                if binary[i] == '1':
                    nexthop.append(self.peer_ip(i))
                    ifname.append(self.port_name(i))

            nexthop = ','.join(nexthop)
            ifname = ','.join(ifname)
            fvs = swss.FieldValuePairs([("nexthop", nexthop), ("ifname", ifname)])
            return fvs

        def asic_route_nhg_fvs(k):
            fvs = asic_db.get_entry(self.ASIC_RT_STR, k)
            if not fvs:
                return None

            print(fvs)
            nhgid = fvs.get("SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID")
            if nhgid is None:
                return None

            fvs = asic_db.get_entry(self.ASIC_NHG_STR, nhgid)
            return fvs

        MAX_ECMP_COUNT = 512
        MAX_PORT_COUNT = 10
        if sys.version_info < (3, 0):
            IP_INTEGER_BASE = int(ipaddress.IPv4Address(unicode("2.2.2.0")))
        else:
            IP_INTEGER_BASE = int(ipaddress.IPv4Address(str("2.2.2.0")))

        for i in range(MAX_PORT_COUNT):
            self.config_intf(i, dvs)

        app_db = dvs.get_app_db()
        asic_db = dvs.get_asic_db()
        ps = swss.ProducerStateTable(app_db.db_connection, "ROUTE_TABLE")

        # Add first batch of routes with unique nexthop groups in AppDB
        route_count = 0
        r = 0
        asic_routes_count = len(asic_db.get_keys(self.ASIC_RT_STR))
        while route_count < MAX_ECMP_COUNT:
            r += 1
            fmt = '{{0:0{}b}}'.format(MAX_PORT_COUNT)
            binary = fmt.format(r)
            # We need at least 2 ports for a nexthop group
            if binary.count('1') <= 1:
                continue
            fvs = gen_nhg_fvs(binary)
            route_ipprefix = gen_ipprefix(route_count)
            ps.set(route_ipprefix, fvs)
            route_count += 1

        # Wait and check ASIC DB the count of nexthop groups used
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, MAX_ECMP_COUNT)

        # Wait and check ASIC DB the count of routes
        asic_db.wait_for_n_keys(self.ASIC_RT_STR, asic_routes_count + MAX_ECMP_COUNT)
        asic_routes_count += MAX_ECMP_COUNT

        # Add a route with labeled NHs
        time.sleep(1)
        asic_nhs_count = len(asic_db.get_keys(self.ASIC_NHS_STR))
        route_ipprefix = gen_ipprefix(route_count)
        fvs = swss.FieldValuePairs([('nexthop', '1+10.0.0.1,3+10.0.0.3'),
                                            ('ifname', 'Ethernet0,Ethernet4')])
        ps.set(route_ipprefix, fvs)
        route_count += 1

        # A temporary route should be created
        asic_db.wait_for_n_keys(self.ASIC_RT_STR, asic_routes_count + 1)

        # A NH should be elected as the temporary NHG and it should be created
        # as it doesn't exist.
        asic_db.wait_for_n_keys(self.ASIC_NHS_STR, asic_nhs_count + 1)

        # Delete the route.  The route and the added labeled NH should be
        # removed.
        ps._del(route_ipprefix)
        route_count -= 1
        asic_db.wait_for_n_keys(self.ASIC_RT_STR, asic_routes_count)
        asic_db.wait_for_n_keys(self.ASIC_NHS_STR, asic_nhs_count)

        # Add second batch of routes with unique nexthop groups in AppDB
        # Add more routes with new nexthop group in AppDBdd
        route_ipprefix = gen_ipprefix(route_count)
        base_ipprefix = route_ipprefix
        base = route_count
        route_count = 0
        while route_count < 10:
            r += 1
            fmt = '{{0:0{}b}}'.format(MAX_PORT_COUNT)
            binary = fmt.format(r)
            # We need at least 2 ports for a nexthop group
            if binary.count('1') <= 1:
                continue
            fvs = gen_nhg_fvs(binary)
            route_ipprefix = gen_ipprefix(base + route_count)
            ps.set(route_ipprefix, fvs)
            route_count += 1
        last_ipprefix = route_ipprefix

        # Wait until we get expected routes and check ASIC DB on the count of nexthop groups used, and it should not increase
        keys = asic_db.wait_for_n_keys(self.ASIC_RT_STR, asic_routes_count + 10)
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, MAX_ECMP_COUNT)

        # Check the route points to next hop group
        # Note: no need to wait here
        k = self.get_route_id("2.2.2.0/24", dvs)
        assert k is not None
        fvs = asic_route_nhg_fvs(k)
        assert fvs is not None

        # Check the second batch does not point to next hop group
        k = self.get_route_id(base_ipprefix, dvs)
        assert k is not None
        fvs = asic_route_nhg_fvs(k)
        assert not(fvs)

        # Remove first batch of routes with unique nexthop groups in AppDB
        route_count = 0
        r = 0
        while route_count < MAX_ECMP_COUNT:
            r += 1
            fmt = '{{0:0{}b}}'.format(MAX_PORT_COUNT)
            binary = fmt.format(r)
            # We need at least 2 ports for a nexthop group
            if binary.count('1') <= 1:
                continue
            route_ipprefix = gen_ipprefix(route_count)
            ps._del(route_ipprefix)
            route_count += 1
        asic_routes_count -= MAX_ECMP_COUNT

        # Wait and check the second batch points to next hop group
        # Check ASIC DB on the count of nexthop groups used, and it should not increase or decrease
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, 10)
        keys = asic_db.get_keys(self.ASIC_RT_STR)
        k = self.get_route_id(base_ipprefix, dvs)
        assert k is not None
        fvs = asic_route_nhg_fvs(k)
        assert fvs is not None
        k = self.get_route_id(last_ipprefix, dvs)
        assert k is not None
        fvs = asic_route_nhg_fvs(k)
        assert fvs is not None

        # Cleanup

        # Remove second batch of routes
        for i in range(10):
            route_ipprefix = gen_ipprefix(MAX_ECMP_COUNT + i)
            ps._del(route_ipprefix)

        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, 0)
        asic_db.wait_for_n_keys(self.ASIC_RT_STR, asic_routes_count)

    def test_nhgorch_nhg_exhaust(self, dvs, testlog):
        app_db = dvs.get_app_db()
        asic_db = dvs.get_asic_db()
        nhg_ps = swss.ProducerStateTable(app_db.db_connection, "NEXT_HOP_GROUP_TABLE")
        rt_ps = swss.ProducerStateTable(app_db.db_connection, "ROUTE_TABLE")

        MAX_ECMP_COUNT = 512
        MAX_PORT_COUNT = 10

        r = 0
        fmt = '{{0:0{}b}}'.format(MAX_PORT_COUNT)
        asic_nhgs_count = len(asic_db.get_keys(self.ASIC_NHG_STR))
        nhg_count = asic_nhgs_count
        first_valid_nhg = nhg_count

        def gen_nhg_fvs(binary):
            nexthop = []
            ifname = []

            for i in range(MAX_PORT_COUNT):
                if binary[i] == '1':
                    nexthop.append(self.peer_ip(i))
                    ifname.append(self.port_name(i))

            nexthop = ','.join(nexthop)
            ifname = ','.join(ifname)
            fvs = swss.FieldValuePairs([("nexthop", nexthop), ("ifname", ifname)])

            return fvs

        def gen_nhg_index(nhg_number):
            return "group{}".format(nhg_number)

        def gen_valid_binary():
            nonlocal r

            while True:
                r += 1
                binary = fmt.format(r)
                # We need at least 2 ports for a nexthop group
                if binary.count('1') <= 1:
                    continue
                return binary

        def create_temp_nhg():
            nonlocal nhg_count

            binary = gen_valid_binary()
            nhg_fvs = gen_nhg_fvs(binary)
            nhg_index = gen_nhg_index(nhg_count)
            nhg_ps.set(nhg_index, nhg_fvs)
            nhg_count += 1

            return nhg_index, binary

        def delete_nhg():
            nonlocal first_valid_nhg

            del_nhg_index = gen_nhg_index(first_valid_nhg)
            del_nhg_id = asic_nhgs[del_nhg_index]

            nhg_ps._del(del_nhg_index)
            asic_nhgs.pop(del_nhg_index)
            first_valid_nhg += 1

            return del_nhg_id

        def nhg_exists(nhg_index):
            return self.get_nhg_id(nhg_index, dvs) is not None

        config_db = dvs.get_config_db()
        fvs = {"NULL": "NULL"}

        # Create interfaces
        for i in range(MAX_PORT_COUNT):
            self.config_intf(i, dvs)

        asic_nhgs = {}

        # Add first batch of next hop groups to reach the NHG limit
        while nhg_count < MAX_ECMP_COUNT:
            r += 1
            binary = fmt.format(r)
            # We need at least 2 ports for a nexthop group
            if binary.count('1') <= 1:
                continue
            nhg_fvs = gen_nhg_fvs(binary)
            nhg_index = gen_nhg_index(nhg_count)
            nhg_ps.set(nhg_index, nhg_fvs)

            # Wait for the group to be added
            asic_db.wait_for_n_keys(self.ASIC_NHG_STR, nhg_count + 1)

            # Save the NHG index/ID pair
            asic_nhgs[nhg_index] = self.get_nhg_id(nhg_index, dvs)

            nhg_count += 1

        # Add a new next hop group - it should create a temporary one instead
        prev_nhgs = asic_db.get_keys(self.ASIC_NHG_STR)
        nhg_index, _ = create_temp_nhg()

        # Assert no new group has been added
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, MAX_ECMP_COUNT)

        # Assert the same NHGs are in ASIC DB
        assert prev_nhgs == asic_db.get_keys(self.ASIC_NHG_STR)

        # Delete an existing next hop group
        del_nhg_id = delete_nhg()

        # Wait for the key to be deleted
        asic_db.wait_for_deleted_entry(self.ASIC_NHG_STR, del_nhg_id)

        # Wait for the temporary group to be promoted and replace the deleted
        # NHG
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, MAX_ECMP_COUNT)

        # Save the promoted NHG index/ID
        asic_nhgs[nhg_index] = self.get_nhg_id(nhg_index, dvs)

        # Update a group
        binary = gen_valid_binary()
        nhg_fvs = gen_nhg_fvs(binary)
        nhg_index = gen_nhg_index(first_valid_nhg)

        # Save the previous members
        prev_nhg_members = self.get_nhgm_ids(nhg_index, dvs)
        nhg_ps.set(nhg_index, nhg_fvs)

        # Wait a second so the NHG members get updated
        time.sleep(1)

        # Assert the group was updated by checking it's members
        assert self.get_nhgm_ids(nhg_index, dvs) != prev_nhg_members

        # Create a new temporary group
        nhg_index, _ = create_temp_nhg()
        time.sleep(1)

        # Delete the temporary group
        nhg_ps._del(nhg_index)

        # Assert the NHG does not exist anymore
        assert not nhg_exists(nhg_index)

        # Assert the number of groups is the same
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, MAX_ECMP_COUNT)

        # Create a new temporary group
        nhg_index, binary = create_temp_nhg()

        # Save the number of group members
        binary_count = binary.count('1')

        # Update the temporary group with a different number of members
        while True:
            r += 1
            binary = fmt.format(r)
            # We need at least 2 ports for a nexthop group
            if binary.count('1') <= 1 or binary.count('1') == binary_count:
                continue
            binary_count = binary.count('1')
            break
        nhg_fvs = gen_nhg_fvs(binary)
        nhg_ps.set(nhg_index, nhg_fvs)

        # Delete a group
        del_nhg_id = delete_nhg()

        # Wait for the group to be deleted
        asic_db.wait_for_deleted_entry(self.ASIC_NHG_STR, del_nhg_id)

        # The temporary group should be promoted
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, MAX_ECMP_COUNT)

        # Save the promoted NHG index/ID
        asic_nhgs[nhg_index] = self.get_nhg_id(nhg_index, dvs)

        # Assert it has the updated details by checking the number of members
        assert len(self.get_nhgm_ids(nhg_index, dvs)) == binary_count

        # Add a route
        asic_rts_count = len(asic_db.get_keys(self.ASIC_RT_STR))

        rt_fvs = swss.FieldValuePairs([('nexthop_group', nhg_index)])
        rt_ps.set('2.2.2.0/24', rt_fvs)

        # Assert the route is created
        asic_db.wait_for_n_keys(self.ASIC_RT_STR, asic_rts_count + 1)

        # Save the previous NHG ID
        prev_nhg_id = asic_nhgs[nhg_index]

        # Create a new temporary group
        nhg_index, binary = create_temp_nhg()

        # Get the route ID
        rt_id = self.get_route_id('2.2.2.0/24', dvs)
        assert rt_id != None

        # Update the route to point to the temporary NHG
        rt_fvs = swss.FieldValuePairs([('nexthop_group', nhg_index)])
        rt_ps.set('2.2.2.0/24', rt_fvs)

        # Wait for the route to change it's NHG ID
        asic_db.wait_for_field_negative_match(self.ASIC_RT_STR,
                                                rt_id,
                                                {'SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID': prev_nhg_id})

        # Save the new route NHG ID
        prev_nhg_id = asic_db.get_entry(self.ASIC_RT_STR, rt_id)['SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID']

        # Update the temporary NHG with one that has different NHs

        # Create a new binary that uses the other interfaces than the previous
        # binary was using
        new_binary = []

        for i in range(len(binary)):
            if binary[i] == '1':
                new_binary.append('0')
            else:
                new_binary.append('1')

        binary = ''.join(new_binary)
        assert binary.count('1') > 1

        nhg_fvs = gen_nhg_fvs(binary)
        nhg_ps.set(nhg_index, nhg_fvs)

        # The NHG ID of the route should change
        asic_db.wait_for_field_negative_match(self.ASIC_RT_STR,
                                                rt_id,
                                                {'SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID': prev_nhg_id})

        # Save the new route NHG ID
        prev_nhg_id = asic_db.get_entry(self.ASIC_RT_STR, rt_id)['SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID']

        # Delete a NHG.
        del_nhg_id = delete_nhg()

        # Wait for the NHG to be deleted
        asic_db.wait_for_deleted_entry(self.ASIC_NHG_STR, del_nhg_id)

        # The temporary group should get promoted.
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, MAX_ECMP_COUNT)

        # Save the promoted NHG index/ID
        asic_nhgs[nhg_index] = self.get_nhg_id(nhg_index, dvs)

        # Assert the NHGID of the route changed due to temporary group being
        # promoted.
        asic_db.wait_for_field_match(self.ASIC_RT_STR,
                                        rt_id,
                                        {'SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID': asic_nhgs[nhg_index]})

        # Try updating the promoted NHG to a single NHG.  Should fail as it no
        # longer is a temporary NHG
        nhg_id = self.get_nhg_id(nhg_index, dvs)
        fvs = swss.FieldValuePairs([('nexthop', '10.0.0.1'),
                                            ('ifname', 'Ethernet0')])
        nhg_ps.set(nhg_index, fvs)
        time.sleep(1)
        assert bool(asic_db.get_entry(self.ASIC_NHG_STR, nhg_id)) == True

        # Save the NHG index
        prev_nhg_index = nhg_index
        prev_nhg_id = nhg_id

        # Create a temporary next hop groups
        dvs.runcmd('swssloglevel -c orchagent -l INFO')
        nhg_index, binary = create_temp_nhg()

        # Update the route to point to the temporary group
        fvs = swss.FieldValuePairs([('nexthop_group', nhg_index)])
        rt_ps.set('2.2.2.0/24', fvs)
        asic_db.wait_for_deleted_entry(self.ASIC_NHG_STR, prev_nhg_id)
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, MAX_ECMP_COUNT)
        nhg_id = self.get_nhg_id(nhg_index, dvs)
        asic_db.wait_for_field_match(self.ASIC_RT_STR,
                                        rt_id,
                                        {'SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID': nhg_id})

        # The previous group should be updated to a single NHG now, freeing a
        # NHG slot in ASIC and promoting the temporary one
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, MAX_ECMP_COUNT)
        asic_nhgs[nhg_index] = self.get_nhg_id(nhg_index, dvs)
        assert self.get_nhg_id(nhg_index, dvs) != prev_nhg_id

        # Create a temporary next hop groups
        nhg_index, binary = create_temp_nhg()
        nhg_id = self.get_nhg_id(nhg_index, dvs)

        # Update the route to point to the temporary group
        fvs = swss.FieldValuePairs([('nexthop_group', nhg_index)])
        rt_ps.set('2.2.2.0/24', fvs)
        asic_db.wait_for_field_match(self.ASIC_RT_STR,
                                        rt_id,
                                        {'SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID': nhg_id})

        # Update the group to a single NHG
        fvs = swss.FieldValuePairs([('nexthop', '10.0.0.1'),
                                            ('ifname', 'Ethernet0')])
        nhg_ps.set(nhg_index, fvs)
        nhg_id = self.get_nhg_id(nhg_index, dvs)

        # Wait for the route to update it's NHG ID
        asic_db.wait_for_field_match(self.ASIC_RT_STR,
                                        rt_id,
                                        {'SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID': nhg_id})

        # Try to update the nexthop group to another one
        fvs = swss.FieldValuePairs([('nexthop', '10.0.0.3'),
                                            ('ifname', 'Ethernet4')])
        nhg_ps.set(nhg_index, fvs)

        # Wait a second so the database operations finish
        time.sleep(1)

        # The update should fail as the group is not temporary anymore and it
        # should keep it's previous NHG ID
        asic_db.wait_for_field_match(self.ASIC_RT_STR,
                                        rt_id,
                                        {'SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID': nhg_id})

        # Create a next hop group that contains labeled NHs that do not exist
        # in NeighOrch
        asic_nhs_count = len(asic_db.get_keys(self.ASIC_NHS_STR))
        fvs = swss.FieldValuePairs([('nexthop', '1+10.0.0.1,3+10.0.0.3'),
                                            ('ifname', 'Ethernet0,Ethernet4')])
        nhg_index = gen_nhg_index(nhg_count)
        nhg_ps.set(nhg_index, fvs)
        nhg_count += 1

        # A temporary next hop should be elected to represent the group and
        # thus a new labeled next hop should be created
        asic_db.wait_for_n_keys(self.ASIC_NHS_STR, asic_nhs_count + 1)

        # Delete a next hop group
        delete_nhg()

        # The group should be promoted and the other labeled NH should also get
        # created
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, MAX_ECMP_COUNT)
        asic_db.wait_for_n_keys(self.ASIC_NHS_STR, asic_nhs_count + 2)

        # Save the promoted NHG index/ID
        asic_nhgs[nhg_index] = self.get_nhg_id(nhg_index, dvs)

        # Update the route with a RouteOrch's owned NHG
        binary = gen_valid_binary()
        nhg_fvs = gen_nhg_fvs(binary)
        rt_ps.set('2.2.2.0/24', nhg_fvs)

        # Assert no new group has been added
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, MAX_ECMP_COUNT)

        # Delete a next hop group
        delete_nhg()

        # The temporary group should be promoted
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, MAX_ECMP_COUNT)

        # Create a temporary NHG owned by NhgOrch
        nhg_index, _ = create_temp_nhg()

        # Assert no new group has been added
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, MAX_ECMP_COUNT)

        # Delete a next hop group
        delete_nhg()

        # Check that the temporary group is promoted
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, MAX_ECMP_COUNT)

        # Save the promoted NHG index/ID
        asic_nhgs[nhg_index] = self.get_nhg_id(nhg_index, dvs)

        # Create a temporary NHG that contains only NHs that do not exist
        nhg_fvs = swss.FieldValuePairs([('nexthop', '10.0.0.21,10.0.0.23'),
                                                ('ifname', 'Ethernet40,Ethernet44')])
        nhg_index = gen_nhg_index(nhg_count)
        nhg_count += 1
        nhg_ps.set(nhg_index, nhg_fvs)

        # Assert the group is not created
        assert not nhg_exists(nhg_index)

        # Update the temporary NHG to a valid one
        binary = gen_valid_binary()
        nhg_fvs = gen_nhg_fvs(binary)
        nhg_ps.set(nhg_index, nhg_fvs)

        # Assert the temporary group was updated and the group got created
        nhg_id = self.get_nhg_id(nhg_index, dvs)
        assert nhg_id is not None

        # Update the temporary NHG to an invalid one again
        nhg_fvs = swss.FieldValuePairs([('nexthop', '10.0.0.21,10.0.0.23'),
                                                ('ifname', 'Ethernet40,Ethernet44')])
        nhg_ps.set(nhg_index, nhg_fvs)

        # The update should fail and the temporary NHG should still be pointing
        # to the old valid NH
        assert self.get_nhg_id(nhg_index, dvs) == nhg_id

        # Delete the temporary group
        nhg_ps._del(nhg_index)

        # Cleanup

        # Delete the route
        rt_ps._del('2.2.2.0/24')
        asic_db.wait_for_deleted_entry(self.ASIC_RT_STR, rt_id)

        # Delete the next hop groups
        for k in asic_nhgs:
            nhg_ps._del(k)
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count)

    def test_nhgorch_multi_nh_group(self, dvs, testlog):
        for i in range(4):
            self.config_intf(i, dvs)

        app_db = dvs.get_app_db()
        asic_db = dvs.get_asic_db()

        # create next hop group in APPL DB

        nhg_ps = swss.ProducerStateTable(app_db.db_connection, "NEXT_HOP_GROUP_TABLE")

        prev_nhg_keys = asic_db.get_keys(self.ASIC_NHG_STR)
        asic_nhgs_count = len(prev_nhg_keys)
        asic_nhgms_count = len(asic_db.get_keys(self.ASIC_NHGM_STR))

        fvs = swss.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3,10.0.0.5'),
                                        ("ifname", "Ethernet0,Ethernet4,Ethernet8")])
        nhg_ps.set("group1", fvs)

        # check if group was propagated to ASIC DB

        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count + 1)
        keys = asic_db.get_keys(self.ASIC_NHG_STR)

        found_nhg = False

        for nhgid in keys:
            if nhgid not in prev_nhg_keys:
                found_nhg = True
                break

        assert found_nhg == True

        # check if members were propagated to ASIC DB

        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, asic_nhgms_count + 3)
        keys = asic_db.get_keys(self.ASIC_NHGM_STR)
        count = 0

        for k in keys:
            fvs = asic_db.get_entry(self.ASIC_NHGM_STR, k)

            if fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"] == nhgid:
                count += 1

        assert count == 3

        # create route in APPL DB

        rt_ps = swss.ProducerStateTable(app_db.db_connection, "ROUTE_TABLE")

        asic_routes_count = len(asic_db.get_keys(self.ASIC_RT_STR))

        fvs = swss.FieldValuePairs([("nexthop_group", "group1")])
        rt_ps.set("2.2.2.0/24", fvs)

        # check if route was propagated to ASIC DB

        asic_db.wait_for_n_keys(self.ASIC_RT_STR, asic_routes_count + 1)
        keys = asic_db.get_keys(self.ASIC_RT_STR)

        k = self.get_route_id('2.2.2.0/24', dvs)
        assert k is not None

        # assert the route points to next hop group
        fvs = asic_db.get_entry(self.ASIC_RT_STR, k)
        assert fvs["SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID"] == nhgid

        # bring links down one-by-one
        for i in [0, 1, 2]:
            self.flap_intf(i, 'down', dvs)

            keys = asic_db.get_keys(self.ASIC_NHGM_STR)

            assert len(keys) == (asic_nhgms_count + 2 - i)
            assert bool(asic_db.get_entry(self.ASIC_NHG_STR, nhgid))

        # bring links up one-by-one
        for i in [0, 1, 2]:
            self.flap_intf(i, 'up', dvs)

            keys = asic_db.get_keys(self.ASIC_NHGM_STR)

            assert len(keys) == (asic_nhgms_count + i + 1)

            count = 0
            for k in keys:
                fvs = asic_db.get_entry(self.ASIC_NHGM_STR, k)
                if fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"] == nhgid:
                    count += 1
            assert count == i + 1

        # Bring an interface down
        self.flap_intf(1, 'down', dvs)

        # One group member will get deleted
        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, 2)

        # Create a group that contains a NH that uses the down link
        fvs = swss.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3'),
                                        ("ifname", "Ethernet0,Ethernet4")])
        nhg_ps.set('group2', fvs)

        # The group should get created, but it will not contained the NH that
        # has the link down
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count + 2)
        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, asic_nhgms_count + 3)

        # Update the NHG with one interface down
        fvs = swss.FieldValuePairs([('nexthop', '10.0.0.3,10.0.0.1'),
                                        ("ifname", "Ethernet4,Ethernet0")])
        nhg_ps.set("group1", fvs)

        # Wait for group members to update - the group will contain only the
        # members that have their links up
        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, asic_nhgms_count + 2)

        # Bring the interface up
        self.flap_intf(1, 'up', dvs)

        # Check that the missing member of group1 and group2 is being added
        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, asic_nhgms_count + 4)

        # Remove group2
        nhg_ps._del('group2')
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count + 1)
        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, asic_nhgms_count + 2)

        # Create group2 with a NH that does not exist
        fvs = swss.FieldValuePairs([('nexthop', '10.0.0.3,10.0.0.63'),
                                        ("ifname", "Ethernet4,Ethernet124")])
        nhg_ps.set("group2", fvs)

        # The groups should not be created
        time.sleep(1)
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count + 1)

        # Update group1 with a NH that does not exist
        fvs = swss.FieldValuePairs([('nexthop', '10.0.0.3,10.0.0.63'),
                                        ("ifname", "Ethernet4,Ethernet124")])
        nhg_ps.set("group1", fvs)

        # The update should fail, leaving group1 with only the unremoved
        # members
        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, asic_nhgms_count + 1)

        # Configure the missing NH's interface
        self.config_intf(31, dvs)

        # A couple more routes will be added to ASIC_DB
        asic_routes_count += 2

        # Group2 should get created and group1 should be updated
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count + 2)
        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, asic_nhgms_count + 4)

        # Delete group2
        nhg_ps._del('group2')
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count + 1)

        # Update the NHG, adding two new members
        fvs = swss.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3,10.0.0.5,10.0.0.7'),
                                        ("ifname", "Ethernet0,Ethernet4,Ethernet8,Ethernet12")])
        nhg_ps.set("group1", fvs)

        # Wait for members to be added
        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, asic_nhgms_count + 4)

        # Update the group to one NH only - orchagent should fail as it is
        # referenced
        fvs = swss.FieldValuePairs([('nexthop', '10.0.0.1'), ("ifname", "Ethernet0")])
        nhg_ps.set("group1", fvs)
        time.sleep(1)

        assert bool(asic_db.get_entry(self.ASIC_NHG_STR, nhgid))
        assert len(asic_db.get_keys(self.ASIC_NHGM_STR)) == asic_nhgms_count + 4

        # Remove route 2.2.2.0/24
        rt_ps._del("2.2.2.0/24")

        # Wait for route 2.2.2.0/24 to be removed
        asic_db.wait_for_n_keys(self.ASIC_RT_STR, asic_routes_count)

        # The group is not referenced anymore, so it should get updated to a
        # single next hop group, removing the ASIC group
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count)
        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, asic_nhgms_count)

        # Remove group1
        nhg_ps._del("group1")

    def test_nhgorch_single_nh_group(self, dvs, testlog):
        for i in range(2):
            self.config_intf(i, dvs)

        app_db = dvs.get_app_db()
        asic_db = dvs.get_asic_db()

        # Count existing objects
        asic_nhgs_count = len(asic_db.get_keys(self.ASIC_NHG_STR))
        asic_nhgms_count = len(asic_db.get_keys(self.ASIC_NHGM_STR))

        # Create single next hop group in APPL DB
        nhg_ps = swss.ProducerStateTable(app_db.db_connection, "NEXT_HOP_GROUP_TABLE")
        fvs = swss.FieldValuePairs([('nexthop', '10.0.0.1'), ("ifname", "Ethernet0")])
        nhg_ps.set("group1", fvs)
        time.sleep(1)

        # Check that the group was not created in ASIC DB
        assert len(asic_db.get_keys(self.ASIC_NHG_STR)) == asic_nhgs_count

        # Get the NHG ID.  The UT assumes there is only one next hop with IP 10.0.0.1
        count = 0
        nhgid = 0
        for k in asic_db.get_keys(self.ASIC_NHS_STR):
            fvs = asic_db.get_entry(self.ASIC_NHS_STR, k)
            if fvs['SAI_NEXT_HOP_ATTR_IP'] == '10.0.0.1':
                nhgid = k
                count += 1
        assert count == 1

        # create route in APPL DB

        rt_ps = swss.ProducerStateTable(app_db.db_connection, "ROUTE_TABLE")

        asic_routes_count = len(asic_db.get_keys(self.ASIC_RT_STR))

        fvs = swss.FieldValuePairs([("nexthop_group", "group1")])
        rt_ps.set("2.2.2.0/24", fvs)

        # check if route was propagated to ASIC DB

        asic_db.wait_for_n_keys(self.ASIC_RT_STR, asic_routes_count + 1)
        keys = asic_db.get_keys(self.ASIC_RT_STR)

        k = self.get_route_id('2.2.2.0/24', dvs)

        # assert the route points to next hop group
        fvs = asic_db.get_entry(self.ASIC_RT_STR, k)
        assert fvs["SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID"] == nhgid

        # Bring group's link down
        self.flap_intf(0, 'down', dvs)

        # Check that the group still has the same ID and everything works
        assert nhgid == self.get_nhg_id('group1', dvs)

        # Bring group's link back up
        self.flap_intf(0, 'up', dvs)

        # Check that the group still has the same ID and everything works
        assert nhgid == self.get_nhg_id('group1', dvs)

        # Bring an interface down
        self.flap_intf(1, 'down', dvs)

        # Create group2 pointing to the NH which's link is down
        fvs = swss.FieldValuePairs([('nexthop', '10.0.0.3'), ("ifname", "Ethernet4")])
        nhg_ps.set("group2", fvs)

        # The group should be created.  To test this, add a route pointing to
        # it.  If the group exists, the route will be created as well.
        fvs = swss.FieldValuePairs([("nexthop_group", "group2")])
        rt_ps.set('2.2.4.0/24', fvs)
        asic_db.wait_for_n_keys(self.ASIC_RT_STR, asic_routes_count + 2)

        # Delete the route and the group
        rt_ps._del('2.2.4.0/24')
        nhg_ps._del('group2')

        # Bring the interface back up
        self.flap_intf(1, 'up', dvs)

        # Create group2 pointing to a NH that does not exist
        fvs = swss.FieldValuePairs([('nexthop', '10.0.0.61'), ("ifname", "Ethernet120")])
        nhg_ps.set("group2", fvs)

        # The group should fail to be created.  To test this, we add a route
        # pointing to it.  The route should not be created.
        fvs = swss.FieldValuePairs([("nexthop_group", "group2")])
        rt_ps.set('2.2.4.0/24', fvs)
        asic_db.wait_for_n_keys(self.ASIC_RT_STR, asic_routes_count + 1)

        # Configure the NH's interface
        self.config_intf(30, dvs)

        # A couple of more routes will be added to ASIC_DB
        asic_routes_count += 2

        # The group should be created, so the route should be added
        asic_db.wait_for_n_keys(self.ASIC_RT_STR, asic_routes_count + 2)

        # Delete the route and the group
        rt_ps._del('2.2.4.0/24')
        nhg_ps._del('group2')

        # Update the group to a multiple NH group - should fail as the group
        # is referenced
        fvs = swss.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3'),
                                          ("ifname", "Ethernet0,Ethernet4")])
        nhg_ps.set("group1", fvs)
        time.sleep(1)
        assert len(asic_db.get_keys(self.ASIC_NHG_STR)) == asic_nhgs_count

        # Update group1 to point to another NH - should fail as the group is
        # referenced
        nhgid = self.get_nhg_id('group1', dvs)
        fvs = swss.FieldValuePairs([('nexthop', '10.0.0.3'), ("ifname", "Ethernet4")])
        nhg_ps.set("group1", fvs)
        assert nhgid == self.get_nhg_id('group1', dvs)

        # Remove route 2.2.2.0/24
        rt_ps._del("2.2.2.0/24")

        # Wait for route 2.2.2.0/24 to be removed
        asic_db.wait_for_n_keys(self.ASIC_RT_STR, asic_routes_count)

        # The group is not referenced anymore, so it should be updated
        assert nhgid != self.get_nhg_id('group1', dvs)

        # Update the group to a multiple NH group - should work as the group is
        # not referenced
        fvs = swss.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3'),
                                          ("ifname", "Ethernet0,Ethernet4")])
        nhg_ps.set("group1", fvs)
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count + 1)

        # Remove group1
        nhg_ps._del("group1")

    def test_nhgorch_label_route(self, dvs, testlog):
        for i in range(4):
            self.config_intf(i, dvs)

        app_db = dvs.get_app_db()
        asic_db = dvs.get_asic_db()

        # create next hop group in APPL DB

        nhg_ps = swss.ProducerStateTable(app_db.db_connection, "NEXT_HOP_GROUP_TABLE")

        prev_nhg_keys = asic_db.get_keys(self.ASIC_NHG_STR)
        asic_nhgs_count = len(prev_nhg_keys)
        asic_nhgms_count = len(asic_db.get_keys(self.ASIC_NHGM_STR))

        fvs = swss.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3,10.0.0.5'),
                                        ("ifname", "Ethernet0,Ethernet4,Ethernet8")])
        nhg_ps.set("group1", fvs)

        # check if group was propagated to ASIC DB

        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count + 1)
        keys = asic_db.get_keys(self.ASIC_NHG_STR)

        found_nhg = False

        for nhgid in keys:
            if nhgid not in prev_nhg_keys:
                found_nhg = True
                break

        assert found_nhg == True

        # check if members were propagated to ASIC DB

        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, asic_nhgms_count + 3)
        keys = asic_db.get_keys(self.ASIC_NHGM_STR)
        count = 0

        for k in keys:
            fvs = asic_db.get_entry(self.ASIC_NHGM_STR, k)

            if fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"] == nhgid:
                count += 1

        assert count == 3

        # create prefix route in APPL DB

        rt_ps = swss.ProducerStateTable(app_db.db_connection, "ROUTE_TABLE")

        asic_routes_count = len(asic_db.get_keys(self.ASIC_RT_STR))

        fvs = swss.FieldValuePairs([("nexthop_group", "group1")])
        rt_ps.set("2.2.2.0/24", fvs)

        # check if route was propagated to ASIC DB

        asic_db.wait_for_n_keys(self.ASIC_RT_STR, asic_routes_count + 1)
        keys = asic_db.get_keys(self.ASIC_RT_STR)

        k = self.get_route_id('2.2.2.0/24', dvs)

        # assert the route points to next hop group
        fvs = asic_db.get_entry(self.ASIC_RT_STR, k)
        assert fvs["SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID"] == nhgid

        # create label route in APPL DB pointing to the same next hop group

        lrt_ps = swss.ProducerStateTable(app_db.db_connection, "LABEL_ROUTE_TABLE")

        asic_insegs_count = len(asic_db.get_keys(self.ASIC_INSEG_STR))

        fvs = swss.FieldValuePairs([("nexthop_group", "group1")])
        lrt_ps.set("20", fvs)

        # check if label route was propagated to ASIC DB

        asic_db.wait_for_n_keys(self.ASIC_INSEG_STR, asic_insegs_count + 1)
        keys = asic_db.get_keys(self.ASIC_INSEG_STR)

        k = self.get_inseg_id('20', dvs)
        assert k is not None

        # assert the route points to next hop group
        fvs = asic_db.get_entry(self.ASIC_INSEG_STR, k)
        assert fvs["SAI_INSEG_ENTRY_ATTR_NEXT_HOP_ID"] == nhgid

        # bring links down one-by-one
        for i in [0, 1, 2]:
            self.flap_intf(i, 'down', dvs)

            keys = asic_db.get_keys(self.ASIC_NHGM_STR)

            assert len(keys) == (asic_nhgms_count + 2 - i)
            assert bool(asic_db.get_entry(self.ASIC_NHG_STR, nhgid))

        # bring links up one-by-one
        for i in [0, 1, 2]:
            self.flap_intf(i, 'up', dvs)

            keys = asic_db.get_keys(self.ASIC_NHGM_STR)

            assert len(keys) == (asic_nhgms_count + i + 1)

            count = 0
            for k in keys:
                fvs = asic_db.get_entry(self.ASIC_NHGM_STR, k)
                if fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"] == nhgid:
                    count += 1
            assert count == i + 1

        # Bring an interface down
        self.flap_intf(1, 'down', dvs)

        # One group member will get deleted
        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, 2)

        # Remove route 2.2.2.0/24
        rt_ps._del("2.2.2.0/24")

        # Wait for route 2.2.2.0/24 to be removed
        asic_db.wait_for_n_keys(self.ASIC_RT_STR, asic_routes_count)

        # Remove label route 20
        lrt_ps._del("20")

        # Wait for route 20 to be removed
        asic_db.wait_for_n_keys(self.ASIC_INSEG_STR, asic_insegs_count)

        # Remove group1
        nhg_ps._del("group1")

        # create label route in APPL DBwith no properties (so just popping the label)

        lrt_ps = swss.ProducerStateTable(app_db.db_connection, "LABEL_ROUTE_TABLE")

        asic_insegs_count = len(asic_db.get_keys(self.ASIC_INSEG_STR))

        fvs = swss.FieldValuePairs([("ifname","")])
        lrt_ps.set("30", fvs)

        # check if label route was propagated to ASIC DB

        asic_db.wait_for_n_keys(self.ASIC_INSEG_STR, asic_insegs_count + 1)
        keys = asic_db.get_keys(self.ASIC_INSEG_STR)

        k = self.get_inseg_id('30', dvs)
        assert k is not None

        # assert the route points to no next hop group
        fvs = asic_db.get_entry(self.ASIC_INSEG_STR, k)
        assert fvs["SAI_INSEG_ENTRY_ATTR_NEXT_HOP_ID"] == 'oid:0x0'

        # Remove label route 30
        lrt_ps._del("30")

        # Wait for route 20 to be removed
        asic_db.wait_for_n_keys(self.ASIC_INSEG_STR, asic_insegs_count)

    def test_cbf_nhgorch(self, dvs, testlog):
        for i in range(4):
            self.config_intf(i, dvs)

        app_db = dvs.get_app_db()
        asic_db = dvs.get_asic_db()
        cbf_nhg_ps = swss.ProducerStateTable(app_db.db_connection, "CLASS_BASED_NEXT_HOP_GROUP_TABLE")
        nhg_ps = swss.ProducerStateTable(app_db.db_connection, "NEXT_HOP_GROUP_TABLE")

        asic_nhgs_count = len(asic_db.get_keys(self.ASIC_NHG_STR))
        asic_nhgms_count = len(asic_db.get_keys(self.ASIC_NHGM_STR))

        # Create two non-CBF nhgs
        fvs = swss.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3,10.0.0.5'),
                                        ("ifname", "Ethernet0,Ethernet4,Ethernet8")])
        nhg_ps.set("group1", fvs)

        fvs = swss.FieldValuePairs([('nexthop', '10.0.0.1,10.0.0.3'),
                                            ('ifname', 'Ethernet0,Ethernet4')])
        nhg_ps.set('group2', fvs)

        # Wait for the groups to appear in ASIC DB
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count + 2)

        # Create a CBF NHG
        fvs = swss.FieldValuePairs([('members', 'group1,group2'),
                        ('class_map', '0:0,1:0,2:0,3:0,4:1,5:1,6:1,7:1')])
        cbf_nhg_ps.set('cbfgroup1', fvs)

        # Check if the group makes it to ASIC DB
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count + 3)
        nhgid = self.get_nhg_id('cbfgroup1', dvs)
        assert(nhgid != None)
        fvs = asic_db.get_entry(self.ASIC_NHG_STR, nhgid)
        assert(fvs['SAI_NEXT_HOP_GROUP_ATTR_TYPE'] == 'SAI_NEXT_HOP_GROUP_TYPE_CLASS_BASED')

        # Check if the next hop group members get created
        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, asic_nhgms_count + 7)
        nhgm_ids = self.get_nhgm_ids('cbfgroup1', dvs)
        assert(len(nhgm_ids) == 2)
        nhg_ids = dict(zip([self.get_nhg_id(x, dvs) for x in ['group1', 'group2']], ['0', '1']))
        for nhgm_id in nhgm_ids:
            fvs = asic_db.get_entry(self.ASIC_NHGM_STR, nhgm_id)
            nh_id = fvs['SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID']
            assert(fvs['SAI_NEXT_HOP_GROUP_MEMBER_ATTR_INDEX'] == nhg_ids[nh_id])
            nhg_ids.pop(nh_id)

        # Delete the CBF next hop group
        cbf_nhg_ps._del('cbfgroup1')
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count + 2)

        # Test the data validation
        fv_values = [
            ('', '0:0,1:1'), # empty members
            ('group1,group2', ''), # empty class map
            ('group1,group1', '0:0,1:1'), # non-unique members
            ('group1,group2', '0;0,1:1'), # ill-formed class map
            ('group1,group2', '-1:0,1:1'), # negative FC value
            ('group1,group2', '64:0,1:1'), # invalid FC value
            ('group1,group2', '0:-1,1:1'), # negative indeex
            ('group1,group2', '0:2,1:1'), # index out of range
            ('group1,group2', '0:0,0:1'), # non-unique class map
            ('group1,group2', 'asd:1,0:0'), # FC not integer
            ('group1,group2', '0:asd,1:1'), # index not integer
            ('group1;group2', '0:0,1:1'), # invalid members separator
            ('group1,group2', '0:0;1:1'), # invalid class map separator
        ]

        for members, class_map in fv_values:
            fvs = swss.FieldValuePairs([('members', members),
                                            ('class_map', class_map)])
            cbf_nhg_ps.set('cbfgroup1', fvs)
            time.sleep(1)
            assert(len(asic_db.get_keys(self.ASIC_NHG_STR)) == asic_nhgs_count + 2)

        # Create a NHG with a single next hop
        fvs = swss.FieldValuePairs([('nexthop', '10.0.0.1'),
                                        ("ifname", "Ethernet0")])
        nhg_ps.set("group3", fvs)
        time.sleep(1)

        # Create a CBF NHG
        fvs = swss.FieldValuePairs([('members', 'group1,group3'),
                                            ('class_map', '0:0,1:1')])
        cbf_nhg_ps.set('cbfgroup1', fvs)
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count + 3)
        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, asic_nhgms_count + 7)\

        # Remove group1 while being referenced - should not work
        nhg_ps._del('group1')
        time.sleep(1)
        assert(len(asic_db.get_keys(self.ASIC_NHG_STR)) == asic_nhgs_count + 3)

        # Update the CBF NHG replacing group1 with group2
        nhgm_ids = self.get_nhgm_ids('cbfgroup1', dvs)
        fvs = swss.FieldValuePairs([('members', 'group2,group3'),
                                            ('class_map', '0:0,1:1')])
        cbf_nhg_ps.set('cbfgroup1', fvs)
        asic_db.wait_for_deleted_keys(self.ASIC_NHGM_STR, nhgm_ids)
        # group1 should be deleted and the updated members added
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count + 2)
        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, asic_nhgms_count + 4)

        # Update the CBF NHG changing the order of the members
        nhgm_ids = self.get_nhgm_ids('cbfgroup1', dvs)
        fvs = swss.FieldValuePairs([('members', 'group3,group2'),
                                            ('class_map', '0:0,1:1')])
        cbf_nhg_ps.set('cbfgroup1', fvs)
        asic_db.wait_for_deleted_keys(self.ASIC_NHGM_STR, nhgm_ids)
        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, asic_nhgms_count + 4)

        indexes = {
            self.get_nhg_id('group3', dvs): '0',
            self.get_nhg_id('group2', dvs): '1'
        }
        nhgm_ids = self.get_nhgm_ids('cbfgroup1', dvs)
        for nhgm_id in nhgm_ids:
            fvs = asic_db.get_entry(self.ASIC_NHGM_STR, nhgm_id)
            nh_id = fvs['SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID']
            assert(fvs['SAI_NEXT_HOP_GROUP_MEMBER_ATTR_INDEX'] == indexes[nh_id])
            indexes.pop(nh_id)

        # Update the CBF NHG class map
        nhg_id = self.get_nhg_id('cbfgroup1', dvs)
        old_map = asic_db.get_entry(self.ASIC_NHG_STR, nhg_id)['SAI_NEXT_HOP_GROUP_ATTR_FORWARDING_CLASS_TO_INDEX_MAP']
        fvs = swss.FieldValuePairs([('members', 'group3,group2'),
                                            ('class_map', '0:1,1:0')])
        cbf_nhg_ps.set('cbfgroup1', fvs)
        asic_db.wait_for_field_negative_match(self.ASIC_NHG_STR,
                                            nhg_id,
                                            {'SAI_NEXT_HOP_GROUP_ATTR_FORWARDING_CLASS_TO_INDEX_MAP': old_map})

        # Create a route pointing to the CBF NHG
        asic_rts_count = len(asic_db.get_keys(self.ASIC_RT_STR))
        rt_ps = swss.ProducerStateTable(app_db.db_connection, swss.APP_ROUTE_TABLE_NAME)
        fvs = swss.FieldValuePairs([('nexthop_group', 'cbfgroup1')])
        rt_ps.set('2.2.2.0/24', fvs)
        asic_db.wait_for_n_keys(self.ASIC_RT_STR, asic_rts_count + 1)

        # Try deleting thee CBF NHG - should not work
        cbf_nhg_ps._del('cbfgroup1')
        time.sleep(1)
        assert(len(asic_db.get_keys(self.ASIC_NHG_STR)) == asic_nhgs_count + 2)

        # Delete the route - the CBF NHG should also get deleted
        rt_ps._del('2.2.2.0/24')
        asic_db.wait_for_n_keys(self.ASIC_RT_STR, asic_rts_count)
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count + 1)
        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, asic_nhgms_count + 2)

        # Create a route pointing to a CBF NHG that does not yet exist
        fvs = swss.FieldValuePairs([('nexthop_group', 'cbfgroup1')])
        rt_ps.set('2.2.2.0/24', fvs)
        time.sleep(1)
        asic_db.wait_for_n_keys(self.ASIC_RT_STR, asic_rts_count)

        # Create the CBF NHG
        fvs = swss.FieldValuePairs([('members', 'group3,group2'),
                                            ('class_map', '0:0,1:1')])
        cbf_nhg_ps.set('cbfgroup1', fvs)
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count + 2)
        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, asic_nhgms_count + 4)
        asic_db.wait_for_n_keys(self.ASIC_RT_STR, asic_rts_count + 1)

        # Try deleting the CBF group
        cbf_nhg_ps._del('cbfgroup1')
        time.sleep(1)
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count + 2)

        # Update the CBF group
        nhgm_ids = self.get_nhgm_ids('cbfgroup1', dvs)
        nhg_id = self.get_nhg_id('cbfgroup1', dvs)
        old_map = asic_db.get_entry(self.ASIC_NHG_STR, nhg_id)['SAI_NEXT_HOP_GROUP_ATTR_FORWARDING_CLASS_TO_INDEX_MAP']
        fvs = swss.FieldValuePairs([('members', 'group2'), ('class_map', '0:0')])
        cbf_nhg_ps.set('cbfgroup1', fvs)
        asic_db.wait_for_deleted_keys(self.ASIC_NHGM_STR, nhgm_ids)
        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, asic_nhgms_count + 3)
        nhgm_id = self.get_nhgm_ids('cbfgroup1', dvs)[0]
        asic_db.wait_for_field_match(self.ASIC_NHGM_STR,
                                     nhgm_id,
                                     {'SAI_NEXT_HOP_GROUP_MEMBER_ATTR_INDEX': '0'})
        asic_db.wait_for_field_negative_match(self.ASIC_NHG_STR,
                                                nhg_id,
            {'SAI_NEXT_HOP_GROUP_ATTR_FORWARDING_CLASS_TO_INDEX_MAP': old_map})

        # Delete the route
        rt_ps._del('2.2.2.0/24')
        asic_db.wait_for_n_keys(self.ASIC_RT_STR, asic_rts_count)
        # The update should have overwritten the delete for CBF NHG
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count + 2)

        # Delete the NHGs
        cbf_nhg_ps._del('cbfgroup1')
        nhg_ps._del('group2')
        nhg_ps._del('group3')
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count)
        asic_db.wait_for_n_keys(self.ASIC_NHGM_STR, asic_nhgms_count)

    def test_cbf_nhg_exhaust(self, dvs, testlog):
        app_db = dvs.get_app_db()
        asic_db = dvs.get_asic_db()
        nhg_ps = swss.ProducerStateTable(app_db.db_connection, swss.APP_NEXT_HOP_GROUP_TABLE_NAME)
        cbf_nhg_ps = swss.ProducerStateTable(app_db.db_connection, swss.APP_CLASS_BASED_NEXT_HOP_GROUP_TABLE_NAME)
        rt_ps = swss.ProducerStateTable(app_db.db_connection, swss.APP_ROUTE_TABLE_NAME)

        MAX_ECMP_COUNT = 512
        MAX_PORT_COUNT = 10

        r = 0
        fmt = '{{0:0{}b}}'.format(MAX_PORT_COUNT)
        asic_nhgs_count = len(asic_db.get_keys(self.ASIC_NHG_STR))
        nhg_count = asic_nhgs_count
        first_valid_nhg = nhg_count

        def gen_nhg_fvs(binary):
            nexthop = []
            ifname = []

            for i in range(MAX_PORT_COUNT):
                if binary[i] == '1':
                    nexthop.append(self.peer_ip(i))
                    ifname.append(self.port_name(i))

            nexthop = ','.join(nexthop)
            ifname = ','.join(ifname)
            fvs = swss.FieldValuePairs([("nexthop", nexthop), ("ifname", ifname)])

            return fvs

        def gen_nhg_index(nhg_number):
            return "group{}".format(nhg_number)

        def gen_valid_binary():
            nonlocal r

            while True:
                r += 1
                binary = fmt.format(r)
                # We need at least 2 ports for a nexthop group
                if binary.count('1') <= 1:
                    continue
                return binary

        def create_temp_nhg():
            nonlocal nhg_count

            binary = gen_valid_binary()
            nhg_fvs = gen_nhg_fvs(binary)
            nhg_index = gen_nhg_index(nhg_count)
            nhg_ps.set(nhg_index, nhg_fvs)
            nhg_count += 1

            return nhg_index, binary

        def delete_nhg():
            nonlocal first_valid_nhg

            del_nhg_index = gen_nhg_index(first_valid_nhg)
            del_nhg_id = asic_nhgs[del_nhg_index]

            nhg_ps._del(del_nhg_index)
            asic_nhgs.pop(del_nhg_index)
            first_valid_nhg += 1

            return del_nhg_id

        def nhg_exists(nhg_index):
            return self.get_nhg_id(nhg_index, dvs) is not None

        # Create interfaces
        for i in range(MAX_PORT_COUNT):
            self.config_intf(i, dvs)

        asic_nhgs = {}

        # Add first batch of next hop groups to reach the NHG limit
        while nhg_count < MAX_ECMP_COUNT:
            r += 1
            binary = fmt.format(r)
            # We need at least 2 ports for a nexthop group
            if binary.count('1') <= 1:
                continue
            nhg_fvs = gen_nhg_fvs(binary)
            nhg_index = gen_nhg_index(nhg_count)
            nhg_ps.set(nhg_index, nhg_fvs)

            # Wait for the group to be added
            asic_db.wait_for_n_keys(self.ASIC_NHG_STR, nhg_count + 1)

            # Save the NHG index/ID pair
            asic_nhgs[nhg_index] = self.get_nhg_id(nhg_index, dvs)

            nhg_count += 1

        # Create a CBF NHG group - it should fail
        fvs = swss.FieldValuePairs([('members', 'group2'),
                                    ('class_map', '0:0')])
        cbf_nhg_ps.set('cbfgroup1', fvs)
        time.sleep(1)
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, MAX_ECMP_COUNT)

        # Delete a NHG
        del_nhg_id = delete_nhg()
        asic_db.wait_for_deleted_entry(self.ASIC_NHG_STR, del_nhg_id)

        # The CBF NHG should be created
        assert(nhg_exists('cbfgroup1'))
        nhg_id = self.get_nhg_id('cbfgroup1', dvs)

        # Create a temporary NHG
        nhg_index, _ = create_temp_nhg()

        # Update the NHG to contain the temporary NHG
        fvs = swss.FieldValuePairs([('members', '{}'.format(nhg_index)),
                                    ('class_map', '0:0')])
        cbf_nhg_ps.set('cbfgroup1', fvs)
        time.sleep(1)
        nhgm_id = self.get_nhgm_ids(None, dvs, nhg_id)[0]
        fvs = asic_db.get_entry(self.ASIC_NHGM_STR, nhgm_id)
        assert(fvs['SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID'] != self.get_nhg_id('group2', dvs))

        # Delete a temporary NHG
        nh_id = self.get_nhg_id(nhg_index, dvs)
        del_nhg_id = delete_nhg()
        asic_db.wait_for_deleted_entry(self.ASIC_NHG_STR, del_nhg_id)

        # The NHG should be promoted and the CBF NHG member updated
        asic_db.wait_for_field_negative_match(self.ASIC_NHGM_STR,
                                                nhgm_id,
                        {'SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID': nh_id})
        asic_nhgs[nhg_index] = self.get_nhg_id(nhg_index, dvs)

        # Create a couple more temporary NHGs
        nhg_index1, _ = create_temp_nhg()
        nhg_index2, _ = create_temp_nhg()

        # Update the CBF NhG to contain both of them as well
        fvs = swss.FieldValuePairs([('members', '{},{},{}'.format(nhg_index, nhg_index1, nhg_index2)),
                                    ('class_map', '0:0,1:1,2:2')])
        cbf_nhg_ps.set('cbfgroup1', fvs)

        # The previous CBF NHG member should be deleted
        asic_db.wait_for_deleted_entry(self.ASIC_NHGM_STR, nhgm_id)
        nhgm_ids = self.get_nhgm_ids(None, dvs, nhg_id)
        values = {}
        for nhgm_id in nhgm_ids:
            fvs = asic_db.get_entry(self.ASIC_NHGM_STR, nhgm_id)
            values[nhgm_id] = fvs['SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID']

        # Update the class map of the group
        old_map = asic_db.get_entry(self.ASIC_NHG_STR, nhg_id)['SAI_NEXT_HOP_GROUP_ATTR_FORWARDING_CLASS_TO_INDEX_MAP']
        members = {nhgm_id: asic_db.get_entry(self.ASIC_NHGM_STR, nhgm_id) for nhgm_id in self.get_nhgm_ids(None, dvs, nhg_id)}
        fvs = swss.FieldValuePairs([('members', '{},{},{}'.format(nhg_index, nhg_index1, nhg_index2)),
                                    ('class_map', '0:1,1:0,2:2')])
        cbf_nhg_ps.set('cbfgroup1', fvs)

        # Check that the map was updates, but the members weren't
        asic_db.wait_for_field_negative_match(self.ASIC_NHG_STR,
                                                nhg_id,
                                                {'SAI_NEXT_HOP_GROUP_ATTR_FORWARDING_CLASS_TO_INDEX_MAP': old_map})
        for nhgm_id in self.get_nhgm_ids(None, dvs, nhg_id):
            assert(members[nhgm_id] == asic_db.get_entry(self.ASIC_NHGM_STR, nhgm_id))

        # Gradually delete temporary NHGs and check that the NHG members are
        # updated
        for i in range(2):
            # Delete a temporary NHG
            del_nhg_id = delete_nhg()
            asic_db.wait_for_deleted_entry(self.ASIC_NHG_STR, del_nhg_id)

            # Check that one of the temporary NHGs has been updated
            diff_count = 0
            for nhgm_id, nh_id in values.items():
                fvs = asic_db.get_entry(self.ASIC_NHGM_STR, nhgm_id)
                if nh_id != fvs['SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID']:
                    values[nhgm_id] = fvs['SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID']
                    diff_count += 1
            assert(diff_count == 1)

        for index in [nhg_index1, nhg_index2]:
            asic_nhgs[index] = self.get_nhg_id(index, dvs)

        # Create a temporary NHG
        nhg_index, binary = create_temp_nhg()

        # Update the CBF NHG to point to the new NHG
        nhgm_ids = self.get_nhgm_ids(None, dvs, nhg_id)
        fvs = swss.FieldValuePairs([('members', nhg_index), ('class_map', '0:0')])
        cbf_nhg_ps.set('cbfgroup1', fvs)
        asic_db.wait_for_deleted_keys(self.ASIC_NHGM_STR, nhgm_ids)

        # Update the temporary NHG to force using a new designated NH
        new_binary = []

        for i in range(len(binary)):
            if binary[i] == '1':
                new_binary.append('0')
            else:
                new_binary.append('1')

        binary = ''.join(new_binary)
        assert binary.count('1') > 1


        nh_id = self.get_nhg_id(nhg_index, dvs)
        nhg_fvs = gen_nhg_fvs(binary)
        nhg_ps.set(nhg_index, nhg_fvs)

        # The CBF NHG member's NH attribute should be updated
        nhgm_id = self.get_nhgm_ids(None, dvs, nhg_id)[0]
        asic_db.wait_for_field_negative_match(self.ASIC_NHGM_STR,
                                                nhgm_id,
                                                {'SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID': nh_id})

        # Delete a NHG
        del_nhg_id = delete_nhg()
        asic_db.wait_for_deleted_entry(self.ASIC_NHG_STR, del_nhg_id)

        # The temporary group should be promoted
        nh_id = self.get_nhg_id(nhg_index, dvs)
        asic_db.wait_for_field_match(self.ASIC_NHGM_STR,
                                    nhgm_id,
                                    {'SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID': nh_id})
        asic_nhgs[nhg_index] = nh_id

        # Delete the NHGs
        cbf_nhg_ps._del('cbfgroup1')
        for k in asic_nhgs:
            nhg_ps._del(k)
        asic_db.wait_for_n_keys(self.ASIC_NHG_STR, asic_nhgs_count)

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass

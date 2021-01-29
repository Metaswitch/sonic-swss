import pytest
import json
import sys
import time

from swsscommon import swsscommon as swss

CFG_DOT1P_TO_TC_MAP_KEY = "AZURE"
DOT1P_TO_TC_MAP = {
    "0": "0",
    "1": "6",
    "2": "5",
    "3": "3",
    "4": "4",
    "5": "2",
    "6": "1",
    "7": "7",
}

CFG_PORT_QOS_MAP_FIELD = "dot1p_to_tc_map"


class TestDot1p(object):
    def connect_dbs(self, dvs):
        self.asic_db = swss.DBConnector(swss.ASIC_DB, dvs.redis_sock, 0)
        self.config_db = swss.DBConnector(swss.CONFIG_DB, dvs.redis_sock, 0)


    def create_dot1p_profile(self):
        tbl = swss.Table(self.config_db, swss.CFG_DOT1P_TO_TC_MAP_TABLE_NAME)
        fvs = swss.FieldValuePairs(list(DOT1P_TO_TC_MAP.items()))
        tbl.set(CFG_DOT1P_TO_TC_MAP_KEY, fvs)
        time.sleep(1)

    def find_dot1p_profile(self):
        found = False
        dot1p_tc_map_raw = None
        dot1p_tc_map_key = None
        tbl = swss.Table(self.asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_QOS_MAP")
        keys = tbl.getKeys()
        for key in keys:
            (status, fvs) = tbl.get(key)
            assert status == True

            for fv in fvs:
                if fv[0] == "SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST":
                    dot1p_tc_map_raw = fv[1]
                elif fv[0] == "SAI_QOS_MAP_ATTR_TYPE" and fv[1] == "SAI_QOS_MAP_TYPE_DOT1P_TO_TC":
                    dot1p_tc_map_key = key
                    found = True

            if found:
                break

        assert found == True

        return (key, dot1p_tc_map_raw)


    def apply_dot1p_profile_on_all_ports(self):
        tbl = swss.Table(self.config_db, swss.CFG_PORT_QOS_MAP_TABLE_NAME)
        fvs = swss.FieldValuePairs([(CFG_PORT_QOS_MAP_FIELD, "[" + swss.CFG_DOT1P_TO_TC_MAP_TABLE_NAME + "|" + CFG_DOT1P_TO_TC_MAP_KEY + "]")])
        ports = swss.Table(self.config_db, swss.CFG_PORT_TABLE_NAME).getKeys()
        for port in ports:
            tbl.set(port, fvs)

        time.sleep(1)


    def test_dot1p_cfg(self, dvs):
        self.connect_dbs(dvs)
        self.create_dot1p_profile()
        oid, dot1p_tc_map_raw = self.find_dot1p_profile()

        dot1p_tc_map = json.loads(dot1p_tc_map_raw)
        for dot1p2tc in dot1p_tc_map['list']:
            dot1p = str(dot1p2tc['key']['dot1p'])
            tc = str(dot1p2tc['value']['tc'])
            assert tc == DOT1P_TO_TC_MAP[dot1p]


    def test_port_dot1p(self, dvs):
        self.connect_dbs(dvs)
        self.create_dot1p_profile()
        oid, dot1p_tc_map_raw = self.find_dot1p_profile()

        self.apply_dot1p_profile_on_all_ports()

        cnt = 0
        tbl = swss.Table(self.asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        keys = tbl.getKeys()
        for key in keys:
            (status, fvs) = tbl.get(key)
            assert status == True

            for fv in fvs:
                if fv[0] == "SAI_PORT_ATTR_QOS_DOT1P_TO_TC_MAP":
                    cnt += 1
                    assert fv[1] == oid

        port_cnt = len(swss.Table(self.config_db, swss.CFG_PORT_TABLE_NAME).getKeys())
        assert port_cnt == cnt

    def test_cbf(self, dvs):
        def get_qos_id():
            nonlocal asic_db
            nonlocal asic_qos_map_ids

            dscp_map_id = None
            for qos_id in asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_QOS_MAP"):
                if qos_id not in asic_qos_map_ids:
                    dscp_map_id = qos_id
                    break
            return dscp_map_id

        cfg_db = dvs.get_config_db()
        asic_db = dvs.get_asic_db()

        dscp_ps = swss.Table(cfg_db.db_connection, swss.CFG_DSCP_TO_FC_MAP_TABLE_NAME)
        exp_ps = swss.Table(cfg_db.db_connection, swss.CFG_EXP_TO_FC_MAP_TABLE_NAME)

        asic_qos_map_ids = asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_QOS_MAP")
        asic_qos_map_count = len(asic_qos_map_ids)

        # Create a DSCP_TO_FC map
        dscp_map = [(str(i), str(i)) for i in range(0, 64)]
        dscp_ps.set("AZURE", swss.FieldValuePairs(dscp_map))

        asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_QOS_MAP", asic_qos_map_count + 1)

        # Get the DSCP map ID
        dscp_map_id = get_qos_id()
        assert(dscp_map_id is not None)

        # Assert the expected values
        fvs = asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_QOS_MAP", dscp_map_id)
        assert(fvs.get("SAI_QOS_MAP_ATTR_TYPE") == "SAI_QOS_MAP_TYPE_DSCP_TO_FORWARDING_CLASS")

        # Delete the map
        dscp_ps._del("AZURE")
        asic_db.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_QOS_MAP", dscp_map_id)

        # Create a map with invalid DSCP values
        dscp_map = [(str(i), str(i)) for i in range(0, 65)]
        dscp_ps.set('AZURE', swss.FieldValuePairs(dscp_map))
        time.sleep(1)
        assert(asic_qos_map_count == len(asic_db.get_keys('ASIC_STATE:SAI_OBJECT_TYPE_QOS_MAP')))

        # Delete the map
        dscp_ps._del("AZURE")

        # Delete a map that does not exist.  Nothing should happen
        dscp_ps._del("AZURE")
        time.sleep(1)
        assert(len(asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_QOS_MAP")) == asic_qos_map_count)

        # Create a EXP_TO_FC map
        exp_map = [(str(i), str(i)) for i in range(0, 8)]
        exp_ps.set("AZURE", swss.FieldValuePairs(exp_map))

        asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_QOS_MAP", asic_qos_map_count + 1)

        # Get the EXP map ID
        exp_map_id = get_qos_id()

        # Assert the expected values
        fvs = asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_QOS_MAP", exp_map_id)
        assert(fvs.get("SAI_QOS_MAP_ATTR_TYPE") == "SAI_QOS_MAP_TYPE_MPLS_EXP_TO_FORWARDING_CLASS")

        # Delete the map
        exp_ps._del("AZURE")
        asic_db.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_QOS_MAP", exp_map_id)

        # Create a map with invalid EXP values
        exp_map = [(str(i), str(i)) for i in range(1, 9)]
        exp_ps.set('AZURE', swss.FieldValuePairs(exp_map))
        time.sleep(1)
        assert(asic_qos_map_count == len(asic_db.get_keys('ASIC_STATE:SAI_OBJECT_TYPE_QOS_MAP')))

        # Delete the map
        exp_ps._del("AZURE")

        # Update the map with valid EXP values but invalid FC
        exp_map = [(str(i), '-1') for i in range(0, 8)]
        exp_ps.set('AZURE', swss.FieldValuePairs(exp_map))
        time.sleep(1)
        assert(asic_qos_map_count == len(asic_db.get_keys('ASIC_STATE:SAI_OBJECT_TYPE_QOS_MAP')))

        # Delete the map
        exp_ps._del("AZURE")

        # Update the map with bigger than unsigned char FC values
        exp_map = [(str(i), '256') for i in range(0, 8)]
        exp_ps.set('AZURE', swss.FieldValuePairs(exp_map))
        time.sleep(1)
        assert(asic_qos_map_count == len(asic_db.get_keys('ASIC_STATE:SAI_OBJECT_TYPE_QOS_MAP')))

        # Delete the map
        exp_ps._del("AZURE")

        # Update the map with valid values
        exp_map = [(str(i), str(i + 10)) for i in range(0, 8)]
        exp_ps.set('AZURE', swss.FieldValuePairs(exp_map))
        asic_db.wait_for_n_keys('ASIC_STATE:SAI_OBJECT_TYPE_QOS_MAP', asic_qos_map_count + 1)

        # Delete the map
        exp_map_id = get_qos_id()
        exp_ps._del("AZURE")
        asic_db.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_QOS_MAP", exp_map_id)

        # Delete a map that does not exist.  Nothing should happen
        exp_ps._del("AZURE")
        time.sleep(1)
        assert(len(asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_QOS_MAP")) == asic_qos_map_count)


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass

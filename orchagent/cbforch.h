#ifndef SWSS_CBFORCH_H
#define SWSS_CBFORCH_H

#include "orch.h"
#include "observer.h"

class CbfOrch : public Orch, public Subject
{
public:
    CbfOrch(DBConnector *db, const vector<table_name_with_pri_t> &tableNames);

private:
    enum MapType
    {
        DSCP,
        EXP
    };

    unordered_map<string, sai_object_id_t> m_dscpMaps;
    unordered_map<string, sai_object_id_t> m_expMaps;

    void doTask(Consumer& consumer);
    void doDscpTask(Consumer& consumer);
    void doExpTask(Consumer& consumer);

    sai_qos_map_list_t extractMap(const KeyOpFieldsValuesTuple &t,
                                  MapType type) const;
    sai_object_id_t addMap(const sai_qos_map_list_t &map_list, MapType type);
    bool updateMap(sai_object_id_t sai_oid,
                   const sai_qos_map_list_t &map_list);
    bool removeMap(sai_object_id_t sai_oid);
};

#endif /* SWSS_CBFORCH_H */

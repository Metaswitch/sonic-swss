#ifndef SWSS_CBFORCH_H
#define SWSS_CBFORCH_H

#include "orch.h"

using namespace std;

/* Class to handle QoS map tasks. */
class MapHandler : public unordered_map<string, sai_object_id_t>
{
public:
    /* The map type handled by this handler. */
    enum class Type
    {
        DSCP,
        EXP
    };

    MapHandler(Type type) : m_type(type) {}

    void doTask(Consumer &consumer);

private:
    Type m_type;

    /* Get the map name based on map type. */
    const char* getMapName() const
                  { return m_type == Type::DSCP ? "DSCP_TO_FC" : "EXP_TO_FC"; }

    sai_qos_map_list_t extractMap(const swss::KeyOpFieldsValuesTuple &t) const;
    sai_object_id_t createMap(const sai_qos_map_list_t &map_list);
    bool updateMap(sai_object_id_t sai_oid,
                   const sai_qos_map_list_t &map_list);
    bool removeMap(sai_object_id_t sai_oid);
};

class CbfOrch : public Orch
{
public:
    CbfOrch(swss::DBConnector *db, const vector<string> &tableNames);

    void doTask(Consumer& consumer) override;

private:
    MapHandler m_dscp_map;
    MapHandler m_exp_map;
};

#endif /* SWSS_CBFORCH_H */

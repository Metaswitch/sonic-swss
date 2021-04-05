#ifndef SWSS_CBFORCH_H
#define SWSS_CBFORCH_H

#include "orch.h"
#include "observer.h"

class CbfOrch : public Orch, public Subject
{
public:
    CbfOrch(DBConnector *db, const vector<table_name_with_pri_t> &tableNames);

private:
    void doTask(Consumer& consumer);
    void doDscpTask(Consumer& consumer);
    void doExpTask(Consumer& consumer);
};

#endif /* SWSS_CBFORCH_H */

#include "cbforch.h"

CbfOrch::CbfOrch(DBConnector *db,
                 const vector<table_name_with_pri_t> &tableNames) :
    Orch(db, tableNames) {}

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
        doDscpTask(consumer);
    }
    else if (table_name == APP_EXP_TO_FC_MAP_TABLE_NAME)
    {
        doExpTask(consumer);
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
void CbfOrch::doDscpTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string index = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {

        }
        else if (op == DEL_COMMAND)
        {

        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

/*
 * Purpose:     Perform the EXP_TO_FC_MAP_TABLE operations.
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
void CbfOrch::doExpTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string index = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {

        }
        else if (op == DEL_COMMAND)
        {

        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define _DEFAULT_SOURCE
#include "os.h"
#include "taoserror.h"
#include "trpc.h"
#include "tsync.h"
#include "treplica.h"
#include "tutil.h"
#include "ttime.h"
#include "tsocket.h"
#include "mgmtDef.h"
#include "mgmtLog.h"
#include "mgmtMnode.h"
#include "mgmtDnode.h"
#include "mgmtSdb.h"
#include "mgmtShell.h"
#include "mgmtUser.h"

int32_t tsMnodeIsMaster = true;
static void *  tsMnodeSdb = NULL;
static int32_t tsMnodeUpdateSize = 0;
static int32_t mgmtGetMnodeMeta(STableMetaMsg *pMeta, SShowObj *pShow, void *pConn);
static int32_t mgmtRetrieveMnodes(SShowObj *pShow, char *data, int32_t rows, void *pConn);

static int32_t mgmtMnodeActionDestroy(SSdbOperDesc *pOper) {
  tfree(pOper->pObj);
  return TSDB_CODE_SUCCESS;
}

static int32_t mgmtMnodeActionInsert(SSdbOperDesc *pOper) {
  SMnodeObj *pMnode = pOper->pObj;
  SDnodeObj *pDnode = mgmtGetDnode(pMnode->mnodeId);
  if (pDnode == NULL) return TSDB_CODE_DNODE_NOT_EXIST;

  pMnode->pDnode = pDnode;
  pDnode->isMgmt = true;
  mgmtReleaseDnode(pDnode);
  
  return TSDB_CODE_SUCCESS;
}

static int32_t mgmtMnodeActionDelete(SSdbOperDesc *pOper) {
  SMnodeObj *pMnode = pOper->pObj;
  mTrace("mnode:%d, is dropped from sdb", pMnode->mnodeId);
  return TSDB_CODE_SUCCESS;
}

static int32_t mgmtMnodeActionUpdate(SSdbOperDesc *pOper) {
  SMnodeObj *pMnode = pOper->pObj;
  SMnodeObj *pSaved = mgmtGetMnode(pMnode->mnodeId);
  if (pMnode != pSaved) {
    memcpy(pSaved, pMnode, pOper->rowSize);
    free(pMnode);
  }

  return TSDB_CODE_SUCCESS;
}

static int32_t mgmtMnodeActionEncode(SSdbOperDesc *pOper) {
  SMnodeObj *pMnode = pOper->pObj;
  memcpy(pOper->rowData, pMnode, tsMnodeUpdateSize);
  pOper->rowSize = tsMnodeUpdateSize;
  return TSDB_CODE_SUCCESS;
}

static int32_t mgmtMnodeActionDecode(SSdbOperDesc *pOper) {
  SMnodeObj *pMnode = calloc(1, sizeof(SMnodeObj));
  if (pMnode == NULL) return TSDB_CODE_SERV_OUT_OF_MEMORY;

  memcpy(pMnode, pOper->rowData, tsMnodeUpdateSize);
  pOper->pObj = pMnode;
  return TSDB_CODE_SUCCESS;
}

static int32_t mgmtMnodeActionRestored() {
  if (mgmtGetMnodesNum() == 1) {
    SMnodeObj *pMnode = NULL;
    mgmtGetNextMnode(NULL, &pMnode);
    if (pMnode != NULL) {
      pMnode->role = TAOS_SYNC_ROLE_MASTER;
      mgmtReleaseMnode(pMnode);
    }
  }
  return TSDB_CODE_SUCCESS;
}

int32_t mgmtInitMnodes() {
  SMnodeObj tObj;
  tsMnodeUpdateSize = (int8_t *)tObj.updateEnd - (int8_t *)&tObj;

  SSdbTableDesc tableDesc = {
    .tableId      = SDB_TABLE_MNODE,
    .tableName    = "mnodes",
    .hashSessions = TSDB_MAX_MNODES,
    .maxRowSize   = tsMnodeUpdateSize,
    .refCountPos  = (int8_t *)(&tObj.refCount) - (int8_t *)&tObj,
    .keyType      = SDB_KEY_INT,
    .insertFp     = mgmtMnodeActionInsert,
    .deleteFp     = mgmtMnodeActionDelete,
    .updateFp     = mgmtMnodeActionUpdate,
    .encodeFp     = mgmtMnodeActionEncode,
    .decodeFp     = mgmtMnodeActionDecode,
    .destroyFp    = mgmtMnodeActionDestroy,
    .restoredFp   = mgmtMnodeActionRestored
  };

  tsMnodeSdb = sdbOpenTable(&tableDesc);
  if (tsMnodeSdb == NULL) {
    mError("failed to init mnodes data");
    return -1;
  }

  mgmtAddShellShowMetaHandle(TSDB_MGMT_TABLE_MNODE, mgmtGetMnodeMeta);
  mgmtAddShellShowRetrieveHandle(TSDB_MGMT_TABLE_MNODE, mgmtRetrieveMnodes);

  mTrace("mnodes table is created");
  return TSDB_CODE_SUCCESS;
}

void mgmtCleanupMnodes() {
  sdbCloseTable(tsMnodeSdb);
}

int32_t mgmtGetMnodesNum() { 
  return sdbGetNumOfRows(tsMnodeSdb); 
}

void *mgmtGetMnode(int32_t mnodeId) {
  return sdbGetRow(tsMnodeSdb, &mnodeId);
}

void mgmtReleaseMnode(SMnodeObj *pMnode) {
  sdbDecRef(tsMnodeSdb, pMnode);
}

void *mgmtGetNextMnode(void *pNode, SMnodeObj **pMnode) { 
  return sdbFetchRow(tsMnodeSdb, pNode, (void **)pMnode); 
}

static char *mgmtGetMnodeRoleStr(int32_t role) {
  switch (role) {
    case TAOS_SYNC_ROLE_OFFLINE:
      return "offline";
    case TAOS_SYNC_ROLE_UNSYNCED:
      return "unsynced";
    case TAOS_SYNC_ROLE_SLAVE:
      return "slave";
    case TAOS_SYNC_ROLE_MASTER:
      return "master";
    default:
      return "undefined";
  }
}

bool mgmtIsMaster() { return tsMnodeIsMaster; }

void mgmtGetMnodeIpList(SRpcIpSet *ipSet, bool usePublicIp) {
  void *pNode = NULL;
  while (1) {
    SMnodeObj *pMnode = NULL;
    pNode = mgmtGetNextMnode(pNode, &pMnode);
    if (pMnode == NULL) break;

    if (usePublicIp) {
      ipSet->ip[ipSet->numOfIps] = htonl(pMnode->pDnode->publicIp);
    } else {
      ipSet->ip[ipSet->numOfIps] = htonl(pMnode->pDnode->privateIp);
    }

    if (pMnode->role == TAOS_SYNC_ROLE_MASTER) {
      ipSet->inUse = ipSet->numOfIps;
    }

    ipSet->numOfIps++;
    ipSet->port = htons(pMnode->pDnode->mnodeShellPort);
    
    mgmtReleaseMnode(pMnode);
  }
}

void mgmtGetMnodeList(void *param) {
  SDMNodeInfos *mnodes = param;
  mnodes->inUse = 0;
  
  int32_t index = 0;
  void *pNode = NULL;
  while (1) {
    SMnodeObj *pMnode = NULL;
    pNode = mgmtGetNextMnode(pNode, &pMnode);
    if (pMnode == NULL) break;

    mnodes->nodeInfos[index].nodeId = htonl(pMnode->mnodeId);
    mnodes->nodeInfos[index].nodeIp = htonl(pMnode->pDnode->privateIp);
    mnodes->nodeInfos[index].nodePort = htons(pMnode->pDnode->mnodeDnodePort);
    strcpy(mnodes->nodeInfos[index].nodeName, pMnode->pDnode->dnodeName);
    mPrint("node:%d role:%s", pMnode->mnodeId, mgmtGetMnodeRoleStr(pMnode->role));
    if (pMnode->role == TAOS_SYNC_ROLE_MASTER) {
      mnodes->inUse = index;
      mPrint("node:%d inUse:%d", pMnode->mnodeId, mnodes->inUse);
    }

    index++;
    mgmtReleaseMnode(pMnode);
  }

  mnodes->nodeNum = index;
}

int32_t mgmtAddMnode(int32_t dnodeId) {
  SMnodeObj *pMnode = calloc(1, sizeof(SMnodeObj));
  pMnode->mnodeId = dnodeId;
  pMnode->createdTime = taosGetTimestampMs();

  SSdbOperDesc oper = {
    .type = SDB_OPER_GLOBAL,
    .table = tsMnodeSdb,
    .pObj = pMnode,
  };

  int32_t code = sdbInsertRow(&oper);
  if (code != TSDB_CODE_SUCCESS) {
    tfree(pMnode);
    code = TSDB_CODE_SDB_ERROR;
  }

  return code;
}

int32_t mgmtDropMnode(int32_t dnodeId) {
  SMnodeObj *pMnode = sdbGetRow(tsMnodeSdb, &dnodeId);
  if (pMnode == NULL) {
    return TSDB_CODE_DNODE_NOT_EXIST;
  }
  
  SSdbOperDesc oper = {
    .type = SDB_OPER_GLOBAL,
    .table = tsMnodeSdb,
    .pObj = pMnode
  };

  int32_t code = sdbDeleteRow(&oper);
  if (code != TSDB_CODE_SUCCESS) {
    code = TSDB_CODE_SDB_ERROR;
  }

  sdbDecRef(tsMnodeSdb, pMnode);
  return code;
}

static int32_t mgmtGetMnodeMeta(STableMetaMsg *pMeta, SShowObj *pShow, void *pConn) {
  SUserObj *pUser = mgmtGetUserFromConn(pConn, NULL);
  if (pUser == NULL) return 0;

  if (strcmp(pUser->pAcct->user, "root") != 0) return TSDB_CODE_NO_RIGHTS;

  int32_t  cols = 0;
  SSchema *pSchema = pMeta->schema;

  pShow->bytes[cols] = 2;
  pSchema[cols].type = TSDB_DATA_TYPE_SMALLINT;
  strcpy(pSchema[cols].name, "id");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 16;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "private ip");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 16;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "public ip");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 8;
  pSchema[cols].type = TSDB_DATA_TYPE_TIMESTAMP;
  strcpy(pSchema[cols].name, "create time");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 10;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "role");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pMeta->numOfColumns = htons(cols);
  pShow->numOfColumns = cols;

  pShow->offset[0] = 0;
  for (int32_t i = 1; i < cols; ++i) {
    pShow->offset[i] = pShow->offset[i - 1] + pShow->bytes[i - 1];
  }

  pShow->numOfRows = mgmtGetMnodesNum();
  pShow->rowSize = pShow->offset[cols - 1] + pShow->bytes[cols - 1];
  pShow->pNode = NULL;
  mgmtReleaseUser(pUser);

  return 0;
}

static int32_t mgmtRetrieveMnodes(SShowObj *pShow, char *data, int32_t rows, void *pConn) {
  int32_t    numOfRows = 0;
  int32_t    cols      = 0;
  SMnodeObj *pMnode   = NULL;
  char      *pWrite;
  char       ipstr[32];

  while (numOfRows < rows) {
    pShow->pNode = mgmtGetNextMnode(pShow->pNode, &pMnode);
    if (pMnode == NULL) break;

    cols = 0;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    *(int16_t *)pWrite = pMnode->mnodeId;
    cols++;

    tinet_ntoa(ipstr, pMnode->pDnode->privateIp);
    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    strcpy(pWrite, ipstr);
    cols++;

    tinet_ntoa(ipstr, pMnode->pDnode->publicIp);
    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    strcpy(pWrite, ipstr);
    cols++;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    *(int64_t *)pWrite = pMnode->createdTime;
    cols++;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    strcpy(pWrite, mgmtGetMnodeRoleStr(pMnode->role));
    cols++;

    numOfRows++;

    mgmtReleaseMnode(pMnode);
  }

  pShow->numOfReads += numOfRows;

  return numOfRows;
}

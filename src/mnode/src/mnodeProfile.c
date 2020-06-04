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
#include "taosmsg.h"
#include "taoserror.h"
#include "tutil.h"
#include "ttime.h"
#include "tcache.h"
#include "tglobal.h"
#include "tdataformat.h"
#include "mnode.h"
#include "mnodeDef.h"
#include "mnodeInt.h"
#include "mnodeAcct.h"
#include "mnodeDnode.h"
#include "mnodeDb.h"
#include "mnodeMnode.h"
#include "mnodeProfile.h"
#include "mnodeShow.h"
#include "mnodeTable.h"
#include "mnodeUser.h"
#include "mnodeVgroup.h"
#include "mnodeWrite.h"

#define CONN_KEEP_TIME (tsShellActivityTimer * 3)
#define CONN_CHECK_TIME (tsShellActivityTimer * 2)

typedef struct {
  char     user[TSDB_USER_LEN + 1];
  int8_t   killed;
  uint16_t port;
  uint32_t ip;
  uint32_t connId;
  uint64_t stime;
} SConnObj;

extern void *tsMnodeTmr;
static SCacheObj *tsMnodeConnCache = NULL;
static uint32_t tsConnIndex = 0;

static int32_t mnodeGetQueryMeta(STableMetaMsg *pMeta, SShowObj *pShow, void *pConn);
static int32_t mnodeRetrieveQueries(SShowObj *pShow, char *data, int32_t rows, void *pConn);
static int32_t mnodeGetConnsMeta(STableMetaMsg *pMeta, SShowObj *pShow, void *pConn);
static int32_t mnodeRetrieveConns(SShowObj *pShow, char *data, int32_t rows, void *pConn);
static int32_t mnodeGetStreamMeta(STableMetaMsg *pMeta, SShowObj *pShow, void *pConn);
static int32_t mnodeRetrieveStreams(SShowObj *pShow, char *data, int32_t rows, void *pConn);

static void    mnodeFreeConn(void *data);
static int32_t mnodeProcessKillQueryMsg(SMnodeMsg *pMsg);
static int32_t mnodeProcessKillStreamMsg(SMnodeMsg *pMsg);
static int32_t mnodeProcessKillConnectionMsg(SMnodeMsg *pMsg);

// static int32_t mnodeKillQuery(char *qidstr, void *pConn);
// static int32_t mnodeKillStream(char *qidstr, void *pConn);

int32_t mnodeInitProfile() {
  mnodeAddShowMetaHandle(TSDB_MGMT_TABLE_QUERIES, mnodeGetQueryMeta);
  mnodeAddShowRetrieveHandle(TSDB_MGMT_TABLE_QUERIES, mnodeRetrieveQueries);
  mnodeAddShowMetaHandle(TSDB_MGMT_TABLE_CONNS, mnodeGetConnsMeta);
  mnodeAddShowRetrieveHandle(TSDB_MGMT_TABLE_CONNS, mnodeRetrieveConns);
  mnodeAddShowMetaHandle(TSDB_MGMT_TABLE_STREAMS, mnodeGetStreamMeta);
  mnodeAddShowRetrieveHandle(TSDB_MGMT_TABLE_STREAMS, mnodeRetrieveStreams);

  mnodeAddWriteMsgHandle(TSDB_MSG_TYPE_CM_KILL_QUERY, mnodeProcessKillQueryMsg);
  mnodeAddWriteMsgHandle(TSDB_MSG_TYPE_CM_KILL_STREAM, mnodeProcessKillStreamMsg);
  mnodeAddWriteMsgHandle(TSDB_MSG_TYPE_CM_KILL_CONN, mnodeProcessKillConnectionMsg);

  tsMnodeConnCache = taosCacheInitWithCb(tsMnodeTmr, CONN_CHECK_TIME, mnodeFreeConn);
  return 0;
}

void mnodeCleanupProfile() {
  if (tsMnodeConnCache != NULL) {
    mPrint("conn cache is cleanup");
    taosCacheCleanup(tsMnodeConnCache);
    tsMnodeConnCache = NULL;
  }
}

uint32_t mnodeCreateConn(char *user, uint32_t ip, uint16_t port) {
  int32_t connSize = taosHashGetSize(tsMnodeConnCache->pHashTable);
  if (connSize > tsMaxShellConns) {
    mError("failed to create conn for user:%s ip:%s:%u, conns:%d larger than maxShellConns:%d, ", user, taosIpStr(ip),
           port, connSize, tsMaxShellConns);
    terrno = TSDB_CODE_TOO_MANY_SHELL_CONNS;
    return 0;
  }

  uint32_t connId = atomic_add_fetch_32(&tsConnIndex, 1);
  if (connId == 0) atomic_add_fetch_32(&tsConnIndex, 1);

  SConnObj connObj = {
    .ip     = ip,
    .port   = port,
    .connId = connId,
    .stime  = taosGetTimestampMs()
  };

  char key[10];
  sprintf(key, "%u", connId);
  strcpy(connObj.user, user);
  void *pConn = taosCachePut(tsMnodeConnCache, key, &connObj, sizeof(connObj), CONN_KEEP_TIME);
  taosCacheRelease(tsMnodeConnCache, &pConn, false);

  mTrace("connId:%d, is created, user:%s ip:%s:%u", connId, user, taosIpStr(ip), port);
  return connId;
}

bool mnodeCheckConn(uint32_t connId, char *user, uint32_t ip, uint16_t port) {
  char key[10];
  sprintf(key, "%u", connId);
  uint64_t expireTime = CONN_KEEP_TIME * 1000 + (uint64_t)taosGetTimestampMs();

  SConnObj *pConn = taosCacheUpdateExpireTimeByName(tsMnodeConnCache, key, expireTime);
  if (pConn == NULL) {
    mError("connId:%d, is already destroyed, user:%s ip:%s:%u", connId, user, taosIpStr(ip), port);
    return false;
  }

  if (pConn->ip != ip || pConn->port != port /* || strcmp(pConn->user, user) != 0 */ ) {
    mError("connId:%d, incoming conn user:%s ip:%s:%u, not match exist conn user:%s ip:%s:%u", connId, user,
           taosIpStr(ip), port, pConn->user, taosIpStr(pConn->ip), pConn->port);
    taosCacheRelease(tsMnodeConnCache, (void**)&pConn, false);       
    return false;
  }

  //mTrace("connId:%d, is incoming, user:%s ip:%s:%u", connId, pConn->user, taosIpStr(pConn->ip), pConn->port);
  taosCacheRelease(tsMnodeConnCache, (void**)&pConn, false);
  return true;
}

static void mnodeFreeConn(void *data) {
  SConnObj *pConn = data;
  mTrace("connId:%d, is destroyed", pConn->connId);
}

static void *mnodeGetNextConn(SHashMutableIterator *pIter, SConnObj **pConn) {
  *pConn = NULL;

  if (pIter == NULL) {
    pIter = taosHashCreateIter(tsMnodeConnCache->pHashTable);
  }

  if (!taosHashIterNext(pIter)) {
    taosHashDestroyIter(pIter);
    return NULL;
  }

  SCacheDataNode **pNode = taosHashIterGet(pIter);
  if (pNode == NULL || *pNode == NULL) {
    taosHashDestroyIter(pIter);
    return NULL;
  }

  *pConn = (SConnObj*)((*pNode)->data);
  return pIter;
}

typedef struct {
  int       numOfConns;
  int       index;
  SConnObj connInfo[];
} SConnShow;

typedef struct {
  uint32_t ip;
  uint16_t port;
  char     user[TSDB_TABLE_ID_LEN+ 1];
} SCDesc;

typedef struct {
  int32_t      index;
  int32_t      numOfQueries;
  SCDesc * connInfo;
  SCDesc **cdesc;
  SQueryDesc   qdesc[];
} SQueryShow;

typedef struct {
  int32_t      index;
  int32_t      numOfStreams;
  SCDesc * connInfo;
  SCDesc **cdesc;
  SStreamDesc   sdesc[];
} SStreamShow;

int32_t  mgmtSaveQueryStreamList(SCMHeartBeatMsg *pHBMsg) {
//  SAcctObj *pAcct = pConn->pAcct;
//
//  if (contLen <= 0 || pAcct == NULL) {
//    return 0;
//  }
//
//  pthread_mutex_lock(&pAcct->mutex);
//
//  if (pConn->pQList) {
//    pAcct->acctInfo.numOfQueries -= pConn->pQList->numOfQueries;
//    pAcct->acctInfo.numOfStreams -= pConn->pSList->numOfStreams;
//  }
//
//  pConn->pQList = realloc(pConn->pQList, contLen);
//  memcpy(pConn->pQList, cont, contLen);
//
//  pConn->pSList = (SStreamList *)(((char *)pConn->pQList) + pConn->pQList->numOfQueries * sizeof(SQueryDesc) + sizeof(SQqueryList));
//
//  pAcct->acctInfo.numOfQueries += pConn->pQList->numOfQueries;
//  pAcct->acctInfo.numOfStreams += pConn->pSList->numOfStreams;
//
//  pthread_mutex_unlock(&pAcct->mutex);

  return TSDB_CODE_SUCCESS;
}

int32_t mnodeGetQueries(SShowObj *pShow, void *pConn) {
//  SAcctObj *  pAcct = pConn->pAcct;
//  SQueryShow *pQueryShow;
//
//  pthread_mutex_lock(&pAcct->mutex);
//
//  pQueryShow = malloc(sizeof(SQueryDesc) * pAcct->acctInfo.numOfQueries + sizeof(SQueryShow));
//  pQueryShow->numOfQueries = 0;
//  pQueryShow->index = 0;
//  pQueryShow->connInfo = NULL;
//  pQueryShow->cdesc = NULL;
//
//  if (pAcct->acctInfo.numOfQueries > 0) {
//    pQueryShow->connInfo = (SCDesc *)malloc(pAcct->acctInfo.numOfConns * sizeof(SCDesc));
//    pQueryShow->cdesc = (SCDesc **)malloc(pAcct->acctInfo.numOfQueries * sizeof(SCDesc *));
//
//    pConn = pAcct->pConn;
//    SQueryDesc * pQdesc = pQueryShow->qdesc;
//    SCDesc * pCDesc = pQueryShow->connInfo;
//    SCDesc **ppCDesc = pQueryShow->cdesc;
//
//    while (pConn) {
//      if (pConn->pQList && pConn->pQList->numOfQueries > 0) {
//        pCDesc->ip = pConn->ip;
//        pCDesc->port = pConn->port;
//        strcpy(pCDesc->user, pConn->pUser->user);
//
//        memcpy(pQdesc, pConn->pQList->qdesc, sizeof(SQueryDesc) * pConn->pQList->numOfQueries);
//        pQdesc += pConn->pQList->numOfQueries;
//        pQueryShow->numOfQueries += pConn->pQList->numOfQueries;
//        for (int32_t i = 0; i < pConn->pQList->numOfQueries; ++i, ++ppCDesc) *ppCDesc = pCDesc;
//
//        pCDesc++;
//      }
//      pConn = pConn->next;
//    }
//  }
//
//  pthread_mutex_unlock(&pAcct->mutex);
//
//  // sorting based on useconds
//
//  pShow->pIter = pQueryShow;

  return 0;
}

int32_t mnodeGetQueryMeta(STableMetaMsg *pMeta, SShowObj *pShow, void *pConn) {
  int32_t cols = 0;

  SSchema *pSchema = pMeta->schema;

  pShow->bytes[cols] = TSDB_USER_LEN;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "user");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = TSDB_IPv4ADDR_LEN + 14;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "ip:port:id");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 8;
  pSchema[cols].type = TSDB_DATA_TYPE_TIMESTAMP;
  strcpy(pSchema[cols].name, "created_time");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 8;
  pSchema[cols].type = TSDB_DATA_TYPE_BIGINT;
  strcpy(pSchema[cols].name, "time(us)");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = TSDB_SHOW_SQL_LEN;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "sql");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pMeta->numOfColumns = htons(cols);
  pShow->numOfColumns = cols;

  pShow->offset[0] = 0;
  for (int32_t i = 1; i < cols; ++i) pShow->offset[i] = pShow->offset[i - 1] + pShow->bytes[i - 1];

  pShow->numOfRows = 1000000;
  pShow->pIter = NULL;
  pShow->rowSize = pShow->offset[cols - 1] + pShow->bytes[cols - 1];

  mnodeGetQueries(pShow, pConn);
  return 0;
}

int32_t mnodeKillQuery(char *qidstr, void *pConn) {
//  char *temp, *chr, idstr[64];
//  strcpy(idstr, qidstr);
//
//  temp = idstr;
//  chr = strchr(temp, ':');
//  if (chr == NULL) goto _error;
//  *chr = 0;
//  uint32_t ip = inet_addr(temp);
//
//  temp = chr + 1;
//  chr = strchr(temp, ':');
//  if (chr == NULL) goto _error;
//  *chr = 0;
//  uint16_t port = htons(atoi(temp));
//
//  temp = chr + 1;
//  uint32_t queryId = atoi(temp);
//
//  SAcctObj *pAcct = pConn->pAcct;
//
//  pthread_mutex_lock(&pAcct->mutex);
//
//  pConn = pAcct->pConn;
//  while (pConn) {
//    if (pConn->ip == ip && pConn->port == port && pConn->pQList) {
//      int32_t     i;
//      SQueryDesc *pQDesc = pConn->pQList->qdesc;
//      for (i = 0; i < pConn->pQList->numOfQueries; ++i, ++pQDesc) {
//        if (pQDesc->queryId == queryId) break;
//      }
//
//      if (i < pConn->pQList->numOfQueries) break;
//    }
//
//    pConn = pConn->next;
//  }
//
//  if (pConn) pConn->queryId = queryId;
//
//  pthread_mutex_unlock(&pAcct->mutex);
//
//  if (pConn == NULL || pConn->pQList == NULL || pConn->pQList->numOfQueries == 0) goto _error;
//
//  mTrace("query:%s is there, kill it", qidstr);
//  return 0;
//
//_error:
//  mTrace("query:%s is not there", qidstr);

  return TSDB_CODE_INVALID_QUERY_ID;
}

int32_t mnodeRetrieveQueries(SShowObj *pShow, char *data, int32_t rows, void *pConn) {
  int32_t   numOfRows = 0;
  char *pWrite;
  int32_t   cols = 0;

  SQueryShow *pQueryShow = (SQueryShow *)pShow->pIter;

  if (rows > pQueryShow->numOfQueries - pQueryShow->index) rows = pQueryShow->numOfQueries - pQueryShow->index;

  while (numOfRows < rows) {
    SQueryDesc *pNode = pQueryShow->qdesc + pQueryShow->index;
    SCDesc *pCDesc = pQueryShow->cdesc[pQueryShow->index];
    cols = 0;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    strcpy(pWrite, pCDesc->user);
    cols++;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    uint32_t ip = pCDesc->ip;
    sprintf(pWrite, "%d.%d.%d.%d:%hu:%d", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, ip >> 24, htons(pCDesc->port),
            pNode->queryId);
    cols++;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    *(int64_t *)pWrite = pNode->stime;
    cols++;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    *(int64_t *)pWrite = pNode->useconds;
    cols++;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    strcpy(pWrite, pNode->sql);
    cols++;

    numOfRows++;
    pQueryShow->index++;
  }

  if (numOfRows == 0) {
    tfree(pQueryShow->cdesc);
    tfree(pQueryShow->connInfo);
    tfree(pQueryShow);
  }

  pShow->numOfReads += numOfRows;
  return numOfRows;
}

int32_t mnodeGetStreams(SShowObj *pShow, void *pConn) {
//  SAcctObj *   pAcct = pConn->pAcct;
//  SStreamShow *pStreamShow;
//
//  pthread_mutex_lock(&pAcct->mutex);
//
//  pStreamShow = malloc(sizeof(SStreamDesc) * pAcct->acctInfo.numOfStreams + sizeof(SQueryShow));
//  pStreamShow->numOfStreams = 0;
//  pStreamShow->index = 0;
//  pStreamShow->connInfo = NULL;
//  pStreamShow->cdesc = NULL;
//
//  if (pAcct->acctInfo.numOfStreams > 0) {
//    pStreamShow->connInfo = (SCDesc *)malloc(pAcct->acctInfo.numOfConns * sizeof(SCDesc));
//    pStreamShow->cdesc = (SCDesc **)malloc(pAcct->acctInfo.numOfStreams * sizeof(SCDesc *));
//
//    pConn = pAcct->pConn;
//    SStreamDesc * pSdesc = pStreamShow->sdesc;
//    SCDesc * pCDesc = pStreamShow->connInfo;
//    SCDesc **ppCDesc = pStreamShow->cdesc;
//
//    while (pConn) {
//      if (pConn->pSList && pConn->pSList->numOfStreams > 0) {
//        pCDesc->ip = pConn->ip;
//        pCDesc->port = pConn->port;
//        strcpy(pCDesc->user, pConn->pUser->user);
//
//        memcpy(pSdesc, pConn->pSList->sdesc, sizeof(SStreamDesc) * pConn->pSList->numOfStreams);
//        pSdesc += pConn->pSList->numOfStreams;
//        pStreamShow->numOfStreams += pConn->pSList->numOfStreams;
//        for (int32_t i = 0; i < pConn->pSList->numOfStreams; ++i, ++ppCDesc) *ppCDesc = pCDesc;
//
//        pCDesc++;
//      }
//      pConn = pConn->next;
//    }
//  }
//
//  pthread_mutex_unlock(&pAcct->mutex);
//
//  // sorting based on useconds
//
//  pShow->pIter = pStreamShow;

  return 0;
}


int32_t mnodeKillStream(char *qidstr, void *pConn) {
//  char *temp, *chr, idstr[64];
//  strcpy(idstr, qidstr);
//
//  temp = idstr;
//  chr = strchr(temp, ':');
//  if (chr == NULL) goto _error;
//  *chr = 0;
//  uint32_t ip = inet_addr(temp);
//
//  temp = chr + 1;
//  chr = strchr(temp, ':');
//  if (chr == NULL) goto _error;
//  *chr = 0;
//  uint16_t port = htons(atoi(temp));
//
//  temp = chr + 1;
//  uint32_t streamId = atoi(temp);
//
//  SAcctObj *pAcct = pConn->pAcct;
//
//  pthread_mutex_lock(&pAcct->mutex);
//
//  pConn = pAcct->pConn;
//  while (pConn) {
//    if (pConn->ip == ip && pConn->port == port && pConn->pSList) {
//      int32_t     i;
//      SStreamDesc *pSDesc = pConn->pSList->sdesc;
//      for (i = 0; i < pConn->pSList->numOfStreams; ++i, ++pSDesc) {
//        if (pSDesc->streamId == streamId) break;
//      }
//
//      if (i < pConn->pSList->numOfStreams) break;
//    }
//
//    pConn = pConn->next;
//  }
//
//  if (pConn) pConn->streamId = streamId;
//
//  pthread_mutex_unlock(&pAcct->mutex);
//
//  if (pConn == NULL || pConn->pSList == NULL || pConn->pSList->numOfStreams == 0) goto _error;
//
//  mTrace("stream:%s is there, kill it", qidstr);
//  return 0;
//
//_error:
//  mTrace("stream:%s is not there", qidstr);

  return TSDB_CODE_INVALID_STREAM_ID;
}

int32_t mnodeGetConnsMeta(STableMetaMsg *pMeta, SShowObj *pShow, void *pConn) {
  int32_t cols = 0;
  SSchema *pSchema = pMeta->schema;

  pShow->bytes[cols] = 4;
  pSchema[cols].type = TSDB_DATA_TYPE_INT;
  strcpy(pSchema[cols].name, "connId");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = TSDB_USER_LEN + VARSTR_HEADER_SIZE;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "user");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = TSDB_IPv4ADDR_LEN + 6 + VARSTR_HEADER_SIZE;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "ip:port");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 8;
  pSchema[cols].type = TSDB_DATA_TYPE_TIMESTAMP;
  strcpy(pSchema[cols].name, "login time");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pMeta->numOfColumns = htons(cols);
  pShow->numOfColumns = cols;

  pShow->offset[0] = 0;
  for (int32_t i = 1; i < cols; ++i) {
    pShow->offset[i] = pShow->offset[i - 1] + pShow->bytes[i - 1];
  }

  pShow->numOfRows = taosHashGetSize(tsMnodeConnCache->pHashTable);
  pShow->rowSize = pShow->offset[cols - 1] + pShow->bytes[cols - 1];

  return 0;
}

int32_t mnodeRetrieveConns(SShowObj *pShow, char *data, int32_t rows, void *pConn) {
  int32_t   numOfRows = 0;
  SConnObj *pConnObj = NULL;
  int32_t   cols = 0;
  char *    pWrite;
  char      ipStr[TSDB_IPv4ADDR_LEN + 7];

  while (numOfRows < rows) {
    pShow->pIter = mnodeGetNextConn(pShow->pIter, &pConnObj);
    if (pConnObj == NULL) break;
    
    cols = 0;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    *(int32_t *) pWrite = pConnObj->connId;
    cols++;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    STR_WITH_MAXSIZE_TO_VARSTR(pWrite, pConnObj->user, TSDB_USER_LEN);
    cols++;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    snprintf(ipStr, TSDB_IPv4ADDR_LEN + 6, "%s:%u", taosIpStr(pConnObj->ip), pConnObj->port);
    STR_WITH_MAXSIZE_TO_VARSTR(pWrite, ipStr, TSDB_IPv4ADDR_LEN + 6);
    cols++;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    *(int64_t *)pWrite = pConnObj->stime;
    cols++;

    numOfRows++;
  }

  pShow->numOfReads += numOfRows;
  const int32_t NUM_OF_COLUMNS = 4;
  mnodeVacuumResult(data, NUM_OF_COLUMNS, numOfRows, rows, pShow);
  
  return numOfRows;
}

static int32_t mnodeGetStreamMeta(STableMetaMsg *pMeta, SShowObj *pShow, void *pConn) {
  return 0;
}

static int32_t mnodeRetrieveStreams(SShowObj *pShow, char *data, int32_t rows, void *pConn) {
  return 0;
}

int32_t mnodeProcessKillQueryMsg(SMnodeMsg *pMsg) {
  // SRpcMsg rpcRsp = {.handle = pMsg->thandle, .pCont = NULL, .contLen = 0, .code = 0, .msgType = 0};
  
  // SUserObj *pUser = mnodeGetUserFromConn(pMsg->thandle);
  // if (pUser == NULL) {
  //   rpcRsp.code = TSDB_CODE_INVALID_USER;
  //   rpcSendResponse(&rpcRsp);
  //   return;
  // }

  // SCMKillQueryMsg *pKill = pMsg->pCont;
  // int32_t code;

  // if (!pUser->writeAuth) {
  //   code = TSDB_CODE_NO_RIGHTS;
  // } else {
  //   code = mgmtKillQuery(pKill->queryId, pMsg->thandle);
  // }

  // rpcRsp.code = code;
  // rpcSendResponse(&rpcRsp);
  // mnodeDecUserRef(pUser);
  return TSDB_CODE_SUCCESS;
}

int32_t mnodeProcessKillStreamMsg(SMnodeMsg *pMsg) {
  // SRpcMsg rpcRsp = {.handle = pMsg->thandle, .pCont = NULL, .contLen = 0, .code = 0, .msgType = 0};
  
  // SUserObj *pUser = mnodeGetUserFromConn(pMsg->thandle);
  // if (pUser == NULL) {
  //   rpcRsp.code = TSDB_CODE_INVALID_USER;
  //   rpcSendResponse(&rpcRsp);
  //   return;
  // }

  // SCMKillStreamMsg *pKill = pMsg->pCont;
  // int32_t code;

  // if (!pUser->writeAuth) {
  //   code = TSDB_CODE_NO_RIGHTS;
  // } else {
  //   code = mgmtKillStream(pKill->queryId, pMsg->thandle);
  // }

  // rpcRsp.code = code;
  // rpcSendResponse(&rpcRsp);
  // mnodeDecUserRef(pUser);
  return TSDB_CODE_SUCCESS;
}

int32_t mnodeProcessKillConnectionMsg(SMnodeMsg *pMsg) {
  SUserObj *pUser = pMsg->pUser;
  if (strcmp(pUser->user, "root") != 0) return TSDB_CODE_NO_RIGHTS;

  SCMKillConnMsg *pKill = pMsg->rpcMsg.pCont;
  SConnObj *      pConn = taosCacheAcquireByName(tsMnodeConnCache, pKill->queryId);
  if (pConn == NULL) {
    mError("connId:%s, failed to kill, conn not exist", pKill->queryId);
    return TSDB_CODE_INVALID_CONNECTION;
  } else {
    mError("connId:%s, is killed by user:%s", pKill->queryId, pUser->user);
    pConn->killed = 1;
    taosCacheRelease(tsMnodeConnCache, (void**)&pConn, false);
    return TSDB_CODE_SUCCESS;
  }
}

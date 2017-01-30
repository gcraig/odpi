//-----------------------------------------------------------------------------
// Copyright (c) 2016, 2017 Oracle and/or its affiliates.  All rights reserved.
// This program is free software: you can modify it and/or redistribute it
// under the terms of:
//
// (i)  the Universal Permissive License v 1.0 or at your option, any
//      later version (http://oss.oracle.com/licenses/upl); and/or
//
// (ii) the Apache License v 2.0. (http://www.apache.org/licenses/LICENSE-2.0)
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// dpiVar.c
//   Implementation of variables.
//-----------------------------------------------------------------------------

#include "dpiImpl.h"

// forward declarations of internal functions only used in this file
static int dpiVar__initBuffers(dpiVar *var, dpiError *error);
static int dpiVar__setBytesFromDynamicBytes(dpiVar *var, dpiBytes *bytes,
        dpiDynamicBytes *dynBytes, dpiError *error);
static int dpiVar__setBytesFromLob(dpiVar *var, dpiBytes *bytes,
        dpiDynamicBytes *dynBytes, dpiLob *lob, dpiError *error);
static int dpiVar__setFromBytes(dpiVar *var, uint32_t pos, const char *value,
        uint32_t valueLength, dpiError *error);
static int dpiVar__setFromLob(dpiVar *var, uint32_t pos, dpiLob *lob,
        dpiError *error);
static int dpiVar__setFromObject(dpiVar *var, uint32_t pos, dpiObject *obj,
        dpiError *error);
static int dpiVar__setFromRowid(dpiVar *var, uint32_t pos, dpiRowid *rowid,
        dpiError *error);
static int dpiVar__setFromStmt(dpiVar *var, uint32_t pos, dpiStmt *stmt,
        dpiError *error);
static int dpiVar__validateTypes(const dpiOracleType *oracleType,
        dpiNativeTypeNum nativeTypeNum, dpiError *error);


//-----------------------------------------------------------------------------
// dpiVar__allocate() [INTERNAL]
//   Create a new variable object and return it. In case of error NULL is
// returned.
//-----------------------------------------------------------------------------
int dpiVar__allocate(dpiConn *conn, dpiOracleTypeNum oracleTypeNum,
        dpiNativeTypeNum nativeTypeNum, uint32_t maxArraySize, uint32_t size,
        int sizeIsBytes, int isArray, dpiObjectType *objType, dpiVar **var,
        dpiData **data, dpiError *error)
{
    const dpiOracleType *type;
    uint32_t sizeInBytes;
    dpiVar *tempVar;

    // validate arguments
    *var = NULL;
    type = dpiOracleType__getFromNum(oracleTypeNum, error);
    if (!type)
        return DPI_FAILURE;
    if (maxArraySize == 0)
        return dpiError__set(error, "check max array size",
                DPI_ERR_ARRAY_SIZE_ZERO);
    if (isArray && !type->canBeInArray)
        return dpiError__set(error, "check can be in array",
                DPI_ERR_NOT_SUPPORTED);
    if (nativeTypeNum != type->defaultNativeTypeNum) {
        if (dpiVar__validateTypes(type, nativeTypeNum, error) < 0)
            return DPI_FAILURE;
    }

    // calculate size in bytes
    if (size == 0)
        size = 1;
    if (type->sizeInBytes)
        sizeInBytes = type->sizeInBytes;
    else if (sizeIsBytes || !type->isCharacterData)
        sizeInBytes = size;
    else if (type->charsetForm == SQLCS_IMPLICIT)
        sizeInBytes = size * conn->env->maxBytesPerCharacter;
    else sizeInBytes = size * conn->env->nmaxBytesPerCharacter;

    // allocate memory for variable type
    if (dpiGen__allocate(DPI_HTYPE_VAR, conn->env, (void**) &tempVar,
            error) < 0)
        return DPI_FAILURE;

    // basic initialization
    tempVar->maxArraySize = maxArraySize;
    tempVar->sizeInBytes = sizeInBytes;
    if (sizeInBytes > DPI_MAX_BASIC_BUFFER_SIZE) {
        tempVar->sizeInBytes = 0;
        tempVar->isDynamic = 1;
        tempVar->requiresPreFetch = 1;
    }
    tempVar->type = type;
    tempVar->nativeTypeNum = nativeTypeNum;
    tempVar->isArray = isArray;
    if (dpiGen__setRefCount(conn, error, 1) < 0) {
        dpiVar__free(tempVar, error);
        return DPI_FAILURE;
    }
    tempVar->conn = conn;
    if (objType) {
        if (dpiGen__checkHandle(objType, DPI_HTYPE_OBJECT_TYPE,
                "check object type", error) < 0) {
            dpiVar__free(tempVar, error);
            return DPI_FAILURE;
        }
        if (dpiGen__setRefCount(objType, error, 1) < 0) {
            dpiVar__free(tempVar, error);
            return DPI_FAILURE;
        }
        tempVar->objectType = objType;
    }

    // allocate the data for the variable
    if (dpiVar__initBuffers(tempVar, error) < 0) {
        dpiVar__free(tempVar, error);
        return DPI_FAILURE;
    }

    *var = tempVar;
    *data = tempVar->externalData;
    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiVar__allocateBuffers() [INTERNAL]
//   Allocate buffers used for passing data to/from Oracle.
//-----------------------------------------------------------------------------
static int dpiVar__allocateBuffers(dpiVar *var, dpiError *error)
{
    uint32_t i, tempBufferSize = 0;
    unsigned long long dataLength;
    dpiBytes *bytes;

    // initialize dynamic buffers for dynamic variables
    if (var->isDynamic) {
        var->dynamicBytes = calloc(var->maxArraySize, sizeof(dpiDynamicBytes));
        if (!var->dynamicBytes)
            return dpiError__set(error, "allocate dynamic bytes",
                    DPI_ERR_NO_MEMORY);

    // for all other variables, validate length and allocate buffers
    } else {
        dataLength = (unsigned long long) var->maxArraySize *
                (unsigned long long) var->sizeInBytes;
        if (dataLength > INT_MAX)
            return dpiError__set(error, "check max array size",
                    DPI_ERR_ARRAY_SIZE_TOO_BIG, var->maxArraySize);
        var->data.asRaw = malloc((size_t) dataLength);
        if (!var->data.asRaw)
            return dpiError__set(error, "allocate buffer", DPI_ERR_NO_MEMORY);
    }

    // allocate the indicator for the variable
    // ensure all values start out as null
    if (!var->indicator) {
        var->indicator = malloc(var->maxArraySize * sizeof(int16_t));
        if (!var->indicator)
            return dpiError__set(error, "allocate indicator",
                    DPI_ERR_NO_MEMORY);
        for (i = 0; i < var->maxArraySize; i++)
            var->indicator[i] = OCI_IND_NULL;
    }

    // allocate the actual length buffers for all but dynamic bytes which are
    // handled differently; ensure actual length starts out as maximum value
    if (!var->isDynamic && !var->actualLength) {
        var->actualLength = (DPI_ACTUAL_LENGTH_TYPE *)
                malloc(var->maxArraySize * sizeof(DPI_ACTUAL_LENGTH_TYPE));
        if (!var->actualLength)
            return dpiError__set(error, "allocate actual length",
                    DPI_ERR_NO_MEMORY);
        for (i = 0; i < var->maxArraySize; i++)
            var->actualLength[i] = var->sizeInBytes;
    }

    // for variable length data, also allocate the return code array
    if (var->type->defaultNativeTypeNum == DPI_NATIVE_TYPE_BYTES &&
            !var->isDynamic && !var->returnCode) {
        var->returnCode = malloc(var->maxArraySize * sizeof(uint16_t));
        if (!var->returnCode)
            return dpiError__set(error, "allocate return code",
                    DPI_ERR_NO_MEMORY);
    }

    // for numbers transferred to/from Oracle as bytes, allocate an additional
    // set of buffers
    if (var->type->oracleTypeNum == DPI_ORACLE_TYPE_NUMBER &&
            var->nativeTypeNum == DPI_NATIVE_TYPE_BYTES) {
        tempBufferSize = DPI_NUMBER_AS_TEXT_CHARS;
        if (var->env->charsetId == DPI_CHARSET_ID_UTF16)
            tempBufferSize *= 2;
        if (!var->tempBuffer) {
            var->tempBuffer = malloc(tempBufferSize * var->maxArraySize);
            if (!var->tempBuffer)
                return dpiError__set(error, "allocate temp buffer",
                        DPI_ERR_NO_MEMORY);
        }
    }

    // allocate the external data array, if needed
    if (!var->externalData) {
        var->externalData = calloc(var->maxArraySize, sizeof(dpiData));
        if (!var->externalData)
            return dpiError__set(error, "allocate external data",
                    DPI_ERR_NO_MEMORY);
        for (i = 0; i < var->maxArraySize; i++)
            var->externalData[i].isNull = 1;
    }

    // for bytes transfers, set encoding and pointers for small strings
    if (var->nativeTypeNum == DPI_NATIVE_TYPE_BYTES) {
        for (i = 0; i < var->maxArraySize; i++) {
            bytes = &var->externalData[i].value.asBytes;
            if (var->type->charsetForm == SQLCS_IMPLICIT)
                bytes->encoding = var->env->encoding;
            else bytes->encoding = var->env->nencoding;
            if (var->tempBuffer)
                bytes->ptr = var->tempBuffer + i * tempBufferSize;
            else if (var->actualLength && !var->dynamicBytes)
                bytes->ptr = var->data.asBytes + i * var->sizeInBytes;
        }
    }

    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiVar__allocateChunks() [INTERNAL]
//   Allocate more chunks for handling dynamic bytes.
//-----------------------------------------------------------------------------
static int dpiVar__allocateChunks(dpiDynamicBytes *dynBytes, dpiError *error)
{
    dpiDynamicBytesChunk *chunks;
    uint32_t allocatedChunks;

    allocatedChunks = dynBytes->allocatedChunks + 8;
    chunks = calloc(allocatedChunks, sizeof(dpiDynamicBytesChunk));
    if (!chunks)
        return dpiError__set(error, "allocate chunks", DPI_ERR_NO_MEMORY);
    if (dynBytes->chunks) {
        memcpy(chunks, dynBytes->chunks,
                dynBytes->numChunks * sizeof(dpiDynamicBytesChunk));
        free(dynBytes->chunks);
    }
    dynBytes->chunks = chunks;
    dynBytes->allocatedChunks = allocatedChunks;
    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiVar__allocateDynamicBytes() [INTERNAL]
//   Allocate space in the dynamic bytes structure for the specified number of
// bytes. When complete, there will be exactly one allocated chunk of the
// specified size or greater in the dynamic bytes structure.
//-----------------------------------------------------------------------------
static int dpiVar__allocateDynamicBytes(dpiDynamicBytes *dynBytes,
        uint32_t size, dpiError *error)
{
    // if an error occurs, none of the original space is valid
    dynBytes->numChunks = 0;

    // if there are no chunks at all, make sure some exist
    if (dynBytes->allocatedChunks == 0 &&
            dpiVar__allocateChunks(dynBytes, error) < 0)
        return DPI_FAILURE;

    // at this point there should be 0 or 1 chunks as any retrieval that
    // resulted in multiple chunks would have been consolidated already
    // make sure that chunk has enough space in it
    if (size > dynBytes->chunks->allocatedLength) {
        if (dynBytes->chunks->ptr)
            free(dynBytes->chunks->ptr);
        dynBytes->chunks->allocatedLength =
                (size + DPI_DYNAMIC_BYTES_CHUNK_SIZE - 1) &
                        ~(DPI_DYNAMIC_BYTES_CHUNK_SIZE - 1);
        dynBytes->chunks->ptr = malloc(dynBytes->chunks->allocatedLength);
        if (!dynBytes->chunks->ptr)
            return dpiError__set(error, "allocate chunk", DPI_ERR_NO_MEMORY);
    }

    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiVar__assignCallbackBuffer() [INTERNAL]
//   Assign callback pointers during OCI statement execution. This is used with
// the callack functions used for dynamic binding during DML returning
// statement execution.
//-----------------------------------------------------------------------------
void dpiVar__assignCallbackBuffer(dpiVar *var, uint32_t index, void **bufpp)
{
    switch (var->type->oracleTypeNum) {
        case DPI_ORACLE_TYPE_TIMESTAMP:
        case DPI_ORACLE_TYPE_TIMESTAMP_TZ:
        case DPI_ORACLE_TYPE_TIMESTAMP_LTZ:
            *bufpp = var->data.asTimestamp[index];
            break;
        case DPI_ORACLE_TYPE_INTERVAL_DS:
        case DPI_ORACLE_TYPE_INTERVAL_YM:
            *bufpp = var->data.asInterval[index];
            break;
        case DPI_ORACLE_TYPE_CLOB:
        case DPI_ORACLE_TYPE_BLOB:
        case DPI_ORACLE_TYPE_NCLOB:
        case DPI_ORACLE_TYPE_BFILE:
            *bufpp = var->data.asLobLocator[index];
            break;
        default:
            *bufpp = var->data.asBytes + index * var->sizeInBytes;
            break;
    }
}


//-----------------------------------------------------------------------------
// dpiVar__checkArraySize() [INTERNAL]
//   Verifies that the array size has not been exceeded.
//-----------------------------------------------------------------------------
static int dpiVar__checkArraySize(dpiVar *var, uint32_t pos,
        const char *fnName, dpiError *error)
{
    if (dpiGen__startPublicFn(var, DPI_HTYPE_VAR, fnName, error) < 0)
        return DPI_FAILURE;
    if (pos >= var->maxArraySize)
        return dpiError__set(error, "check array size",
                DPI_ERR_ARRAY_SIZE_EXCEEDED, var->maxArraySize, pos);
    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiVar__convertToLob() [INTERNAL]
//   Convert the variable from using dynamic bytes for a long string to using a
// LOB instead. This is needed for PL/SQL which cannot handle more than 32K
// without the use of a LOB.
//-----------------------------------------------------------------------------
int dpiVar__convertToLob(dpiVar *var, dpiError *error)
{
    dpiDynamicBytes *dynBytes;
    dpiLob *lob;
    uint32_t i;

    // change type based on the original Oracle type
    if (var->type->oracleTypeNum == DPI_ORACLE_TYPE_RAW ||
            var->type->oracleTypeNum == DPI_ORACLE_TYPE_LONG_RAW)
        var->type = dpiOracleType__getFromNum(DPI_ORACLE_TYPE_BLOB, error);
    else if (var->type->oracleTypeNum == DPI_ORACLE_TYPE_NCHAR ||
            var->type->oracleTypeNum == DPI_ORACLE_TYPE_LONG_NVARCHAR)
        var->type = dpiOracleType__getFromNum(DPI_ORACLE_TYPE_NCLOB,
                error);
    else var->type = dpiOracleType__getFromNum(DPI_ORACLE_TYPE_CLOB,
            error);

    // adjust attributes and re-initialize buffers
    // the dynamic bytes structures will not be removed
    var->sizeInBytes = var->type->sizeInBytes;
    var->isDynamic = 0;
    if (dpiVar__initBuffers(var, error) < 0)
        return DPI_FAILURE;

    // copy any values already set
    for (i = 0; i < var->maxArraySize; i++) {
        dynBytes = &var->dynamicBytes[i];
        lob = var->references[i].asLOB;
        if (dynBytes->numChunks == 0)
            continue;
        if (dpiLob__setFromBytes(lob, dynBytes->chunks->ptr,
                dynBytes->chunks->length, error) < 0)
            return DPI_FAILURE;
    }

    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiVar__copyData() [PUBLIC]
//   Copy the data from the source to the target variable at the given array
// position.
//-----------------------------------------------------------------------------
int dpiVar__copyData(dpiVar *var, uint32_t pos, dpiData *sourceData,
        dpiError *error)
{
    dpiData *targetData = &var->externalData[pos];

    // handle null case
    targetData->isNull = sourceData->isNull;
    if (sourceData->isNull)
        return DPI_SUCCESS;

    // handle copying of value from source to target
    switch (var->nativeTypeNum) {
        case DPI_NATIVE_TYPE_BYTES:
            return dpiVar__setFromBytes(var, pos,
                    sourceData->value.asBytes.ptr,
                    sourceData->value.asBytes.length, error);
        case DPI_NATIVE_TYPE_LOB:
            return dpiVar__setFromLob(var, pos, sourceData->value.asLOB,
                    error);
        case DPI_NATIVE_TYPE_OBJECT:
            return dpiVar__setFromObject(var, pos, sourceData->value.asObject,
                    error);
        case DPI_NATIVE_TYPE_STMT:
            return dpiVar__setFromStmt(var, pos, sourceData->value.asStmt,
                    error);
        case DPI_NATIVE_TYPE_ROWID:
            return dpiVar__setFromRowid(var, pos, sourceData->value.asRowid,
                    error);
        default:
            memcpy(targetData, sourceData, sizeof(dpiData));
    }

    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiVar__defineCallback() [INTERNAL]
//   Callback which runs during OCI statement execution and allocates the
// buffers required as well as provides that information to the OCI. This is
// intended for handling string and raw columns for which the size is unknown.
// These include LONG, LONG RAW and retrieving CLOB and BLOB as bytes, rather
// than use the LOB API.
//-----------------------------------------------------------------------------
int32_t dpiVar__defineCallback(dpiVar *var, OCIDefine *defnp, uint32_t iter,
        void **bufpp, uint32_t **alenpp, uint8_t *piecep, void **indpp,
        uint16_t **rcodepp)
{
    dpiDynamicBytesChunk *chunk;
    dpiDynamicBytes *bytes;

    // allocate more chunks, if necessary
    bytes = &var->dynamicBytes[iter];
    if (bytes->numChunks == bytes->allocatedChunks &&
            dpiVar__allocateChunks(bytes, var->error) < 0)
        return DPI_FAILURE;

    // allocate memory for the chunk, if needed
    chunk = &bytes->chunks[bytes->numChunks];
    if (!chunk->ptr) {
        chunk->allocatedLength = DPI_DYNAMIC_BYTES_CHUNK_SIZE;
        chunk->ptr = malloc(chunk->allocatedLength);
        if (!chunk->ptr) {
            dpiError__set(var->error, "allocate buffer", DPI_ERR_NO_MEMORY);
            return OCI_ERROR;
        }
    }

    // return chunk to OCI
    bytes->numChunks++;
    chunk->length = chunk->allocatedLength;
    *bufpp = chunk->ptr;
    *alenpp = &chunk->length;
    *indpp = &(var->indicator[iter]);
    *rcodepp = NULL;
    return OCI_CONTINUE;
}


//-----------------------------------------------------------------------------
// dpiVar__extendedInitialize() [INTERNAL]
//   Performs extended initialization specific to each variable type.
//-----------------------------------------------------------------------------
static int dpiVar__extendedInitialize(dpiVar *var, dpiError *error)
{
    sword status;

    // create array of references, if applicable
    if (var->type->requiresPreFetch && !var->isDynamic) {
        var->references = calloc(var->maxArraySize,
                sizeof(dpiReferenceBuffer));
        if (!var->references)
            return dpiError__set(error, "allocate references",
                    DPI_ERR_NO_MEMORY);
    }

    // perform variable specific initialization
    switch (var->type->oracleTypeNum) {
        case DPI_ORACLE_TYPE_TIMESTAMP:
            status = OCIArrayDescriptorAlloc(var->env->handle,
                    (void**) &var->data.asTimestamp[0], OCI_DTYPE_TIMESTAMP,
                    var->maxArraySize, 0, NULL);
            break;
        case DPI_ORACLE_TYPE_TIMESTAMP_LTZ:
            status = OCIArrayDescriptorAlloc(var->env->handle,
                    (void**) &var->data.asTimestamp[0],
                    OCI_DTYPE_TIMESTAMP_LTZ, var->maxArraySize, 0, NULL);
            break;
        case DPI_ORACLE_TYPE_TIMESTAMP_TZ:
            status = OCIArrayDescriptorAlloc(var->env->handle,
                    (void**) &var->data.asTimestamp[0],
                    OCI_DTYPE_TIMESTAMP_TZ, var->maxArraySize, 0, NULL);
            break;
        case DPI_ORACLE_TYPE_INTERVAL_DS:
            status = OCIArrayDescriptorAlloc(var->env->handle,
                    (void**) &var->data.asInterval[0], OCI_DTYPE_INTERVAL_DS,
                    var->maxArraySize, 0, NULL);
            break;
        case DPI_ORACLE_TYPE_INTERVAL_YM:
            status = OCIArrayDescriptorAlloc(var->env->handle,
                    (void**) &var->data.asInterval[0], OCI_DTYPE_INTERVAL_YM,
                    var->maxArraySize, 0, NULL);
            break;
        case DPI_ORACLE_TYPE_CLOB:
        case DPI_ORACLE_TYPE_BLOB:
        case DPI_ORACLE_TYPE_NCLOB:
        case DPI_ORACLE_TYPE_BFILE:
        case DPI_ORACLE_TYPE_STMT:
        case DPI_ORACLE_TYPE_ROWID:
            return dpiVar__extendedPreFetch(var, error);
        case DPI_ORACLE_TYPE_OBJECT:
            if (!var->objectType)
                return dpiError__set(error, "check object type",
                        DPI_ERR_NO_OBJECT_TYPE);
            var->objectIndicator = malloc(var->maxArraySize * sizeof(void*));
            if (!var->objectIndicator)
                return dpiError__set(error, "allocate object indicator",
                        DPI_ERR_NO_MEMORY);
            return dpiVar__extendedPreFetch(var, error);
        default:
            return DPI_SUCCESS;
    }
    return dpiError__check(error, status, var->conn, "allocate descriptors");
}


//-----------------------------------------------------------------------------
// dpiVar__extendedPreFetch() [INTERNAL]
//   Perform any necessary actions prior to fetching data.
//-----------------------------------------------------------------------------
int dpiVar__extendedPreFetch(dpiVar *var, dpiError *error)
{
    dpiRowid *rowid;
    dpiData *data;
    dpiStmt *stmt;
    sword status;
    dpiLob *lob;
    uint32_t i;

    if (var->isDynamic) {
        for (i = 0; i < var->maxArraySize; i++)
            var->dynamicBytes[i].numChunks = 0;
        return DPI_SUCCESS;
    }

    switch (var->type->oracleTypeNum) {
        case DPI_ORACLE_TYPE_STMT:
            for (i = 0; i < var->maxArraySize; i++) {
                data = &var->externalData[i];
                if (var->references[i].asStmt) {
                    dpiGen__setRefCount(var->references[i].asStmt, error, -1);
                    var->references[i].asStmt = NULL;
                }
                var->data.asStmt[i] = NULL;
                data->value.asStmt = NULL;
                if (dpiStmt__allocate(var->conn, 0, &stmt, error) < 0)
                    return DPI_FAILURE;
                var->references[i].asStmt = stmt;
                status = OCIHandleAlloc(var->env->handle,
                        (dvoid**) &stmt->handle, OCI_HTYPE_STMT, 0, 0);
                if (dpiError__check(error, status, var->conn,
                        "allocate statement") < 0)
                    return DPI_FAILURE;
                stmt->isOwned = 1;
                var->data.asStmt[i] = stmt->handle;
                data->value.asStmt = stmt;
            }
            break;
        case DPI_ORACLE_TYPE_CLOB:
        case DPI_ORACLE_TYPE_BLOB:
        case DPI_ORACLE_TYPE_NCLOB:
        case DPI_ORACLE_TYPE_BFILE:
            for (i = 0; i < var->maxArraySize; i++) {
                data = &var->externalData[i];
                if (var->references[i].asLOB) {
                    dpiGen__setRefCount(var->references[i].asLOB, error, -1);
                    var->references[i].asLOB = NULL;
                }
                var->data.asLobLocator[i] = NULL;
                data->value.asLOB = NULL;
                if (dpiLob__allocate(var->conn, var->type, &lob, error) < 0)
                    return DPI_FAILURE;
                var->references[i].asLOB = lob;
                var->data.asLobLocator[i] = lob->locator;
                data->value.asLOB = lob;
                if (var->dynamicBytes &&
                        dpiLob__createTemporary(lob, error) < 0)
                    return DPI_FAILURE;
            }
            break;
        case DPI_ORACLE_TYPE_ROWID:
            for (i = 0; i < var->maxArraySize; i++) {
                data = &var->externalData[i];
                if (var->references[i].asRowid) {
                    dpiGen__setRefCount(var->references[i].asRowid, error, -1);
                    var->references[i].asRowid = NULL;
                }
                var->data.asRowid[i] = NULL;
                data->value.asRowid = NULL;
                if (dpiRowid__allocate(var->conn, &rowid, error) < 0)
                    return DPI_FAILURE;
                var->references[i].asRowid = rowid;
                var->data.asRowid[i] = rowid->handle;
                data->value.asRowid = rowid;
            }
            break;
        case DPI_ORACLE_TYPE_OBJECT:
            for (i = 0; i < var->maxArraySize; i++) {
                data = &var->externalData[i];
                if (var->references[i].asObject) {
                    dpiGen__setRefCount(var->references[i].asObject, error,
                            -1);
                    var->references[i].asObject = NULL;
                }
                var->data.asObject[i] = NULL;
                var->objectIndicator[i] = NULL;
                data->value.asObject = NULL;
            }
            break;
        default:
            break;
    }

    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiVar__finalizeBuffers() [INTERNAL]
//   Finalize buffers used for passing data to/from Oracle.
//-----------------------------------------------------------------------------
static void dpiVar__finalizeBuffers(dpiVar *var, dpiError *error)
{
    dpiDynamicBytes *dynBytes;
    uint32_t i, j;

    // free any descriptors that were created
    switch (var->type->oracleTypeNum) {
        case DPI_ORACLE_TYPE_TIMESTAMP:
            OCIArrayDescriptorFree((void**) &var->data.asTimestamp[0],
                    OCI_DTYPE_TIMESTAMP);
            break;
        case DPI_ORACLE_TYPE_TIMESTAMP_TZ:
            OCIArrayDescriptorFree((void**) &var->data.asTimestamp[0],
                    OCI_DTYPE_TIMESTAMP_TZ);
            break;
        case DPI_ORACLE_TYPE_TIMESTAMP_LTZ:
            OCIArrayDescriptorFree((void**) &var->data.asTimestamp[0],
                    OCI_DTYPE_TIMESTAMP_LTZ);
            break;
        case DPI_ORACLE_TYPE_INTERVAL_DS:
            OCIArrayDescriptorFree((void**) &var->data.asInterval[0],
                    OCI_DTYPE_INTERVAL_DS);
            break;
        case DPI_ORACLE_TYPE_INTERVAL_YM:
            OCIArrayDescriptorFree((void**) &var->data.asInterval[0],
                    OCI_DTYPE_INTERVAL_YM);
            break;
        case DPI_ORACLE_TYPE_ROWID:
            OCIArrayDescriptorFree((void**) &var->data.asRowid[0],
                    OCI_DTYPE_ROWID);
            break;
        default:
            break;
    }

    // release any references that were created
    if (var->references) {
        for (i = 0; i < var->maxArraySize; i++) {
            if (var->references[i].asHandle) {
                dpiGen__setRefCount(var->references[i].asHandle, error, -1);
                var->references[i].asHandle = NULL;
            }
        }
        free(var->references);
        var->references = NULL;
    }

    // free any dynamic buffers
    if (var->dynamicBytes) {
        for (i = 0; i < var->maxArraySize; i++) {
            dynBytes = &var->dynamicBytes[i];
            if (dynBytes->allocatedChunks > 0) {
                for (j = 0; j < dynBytes->allocatedChunks; j++) {
                    if (dynBytes->chunks[j].ptr) {
                        free(dynBytes->chunks[j].ptr);
                        dynBytes->chunks[j].ptr = NULL;
                    }
                }
                free(dynBytes->chunks);
                dynBytes->allocatedChunks = 0;
                dynBytes->chunks = NULL;
            }
        }
        free(var->dynamicBytes);
        var->dynamicBytes = NULL;
    }

    // free other memory allocated
    if (var->indicator) {
        free(var->indicator);
        var->indicator = NULL;
    }
    if (var->returnCode) {
        free(var->returnCode);
        var->returnCode = NULL;
    }
    if (var->actualLength) {
        free(var->actualLength);
        var->actualLength = NULL;
    }
#if DPI_ORACLE_CLIENT_VERSION_HEX < DPI_ORACLE_CLIENT_VERSION(12,1)
    if (var->dynamicActualLength) {
        free(var->dynamicActualLength);
        var->dynamicActualLength = NULL;
    }
#endif
    if (var->externalData) {
        free(var->externalData);
        var->externalData = NULL;
    }
    if (var->data.asRaw) {
        free(var->data.asRaw);
        var->data.asRaw = NULL;
    }
    if (var->objectIndicator) {
        free(var->objectIndicator);
        var->objectIndicator = NULL;
    }
    if (var->tempBuffer) {
        free(var->tempBuffer);
        var->tempBuffer = NULL;
    }
}


//-----------------------------------------------------------------------------
// dpiVar__free() [INTERNAL]
//   Free the memory associated with the variable.
//-----------------------------------------------------------------------------
void dpiVar__free(dpiVar *var, dpiError *error)
{
    dpiVar__finalizeBuffers(var, error);
    if (var->objectType) {
        dpiGen__setRefCount(var->objectType, error, -1);
        var->objectType = NULL;
    }
    if (var->conn) {
        dpiGen__setRefCount(var->conn, error, -1);
        var->conn = NULL;
    }
    free(var);
}


//-----------------------------------------------------------------------------
// dpiVar__getValue() [PRIVATE]
//   Returns the contents of the variable in the type specified, if possible.
//-----------------------------------------------------------------------------
int dpiVar__getValue(dpiVar *var, uint32_t pos, dpiData *data,
        dpiError *error)
{
    dpiOracleTypeNum oracleTypeNum;
    dpiBytes *bytes;

    // check for a NULL value; for objects the indicator is elsewhere
    if (!var->objectIndicator)
        data->isNull = (var->indicator[pos] == OCI_IND_NULL);
    else if (var->objectIndicator[pos])
        data->isNull =
                (*((OCIInd*) var->objectIndicator[pos]) == OCI_IND_NULL);
    else data->isNull = 1;
    if (data->isNull)
        return DPI_SUCCESS;

    // check return code for variable length data
    if (var->returnCode) {
        if (var->returnCode[pos] != 0) {
            dpiError__set(error, "check return code", DPI_ERR_COLUMN_FETCH,
                    pos, var->returnCode[pos]);
            error->buffer->code = var->returnCode[pos];
            return DPI_FAILURE;
        }
    }

#if DPI_ORACLE_CLIENT_VERSION_HEX < DPI_ORACLE_CLIENT_VERSION(12,1)
    // for 11g, dynamic lengths are 32-bit whereas static lengths are 16-bit
    if (var->dynamicActualLength)
        var->actualLength[pos] = (uint16_t) var->dynamicActualLength[pos];
#endif

    // transform the various types
    oracleTypeNum = var->type->oracleTypeNum;
    switch (var->nativeTypeNum) {
        case DPI_NATIVE_TYPE_INT64:
        case DPI_NATIVE_TYPE_UINT64:
            switch (oracleTypeNum) {
                case DPI_ORACLE_TYPE_NATIVE_INT:
                    data->value.asInt64 = var->data.asInt64[pos];
                    return DPI_SUCCESS;
                case DPI_ORACLE_TYPE_NUMBER:
                    return dpiData__fromOracleNumberAsInteger(data, var->env,
                            error, &var->data.asNumber[pos]);
                default:
                    break;
            }
            break;
        case DPI_NATIVE_TYPE_DOUBLE:
            switch (oracleTypeNum) {
                case DPI_ORACLE_TYPE_NUMBER:
                    return dpiData__fromOracleNumberAsDouble(data, var->env,
                            error, &var->data.asNumber[pos]);
                case DPI_ORACLE_TYPE_NATIVE_DOUBLE:
                    data->value.asDouble = var->data.asDouble[pos];
                    return DPI_SUCCESS;
                case DPI_ORACLE_TYPE_TIMESTAMP:
                case DPI_ORACLE_TYPE_TIMESTAMP_TZ:
                case DPI_ORACLE_TYPE_TIMESTAMP_LTZ:
                    return dpiData__fromOracleTimestampAsDouble(data, var->env,
                            error, var->data.asTimestamp[pos]);
                default:
                    break;
            }
            break;
        case DPI_NATIVE_TYPE_BYTES:
            bytes = &data->value.asBytes;
            switch (oracleTypeNum) {
                case DPI_ORACLE_TYPE_VARCHAR:
                case DPI_ORACLE_TYPE_NVARCHAR:
                case DPI_ORACLE_TYPE_CHAR:
                case DPI_ORACLE_TYPE_NCHAR:
                case DPI_ORACLE_TYPE_ROWID:
                case DPI_ORACLE_TYPE_RAW:
                case DPI_ORACLE_TYPE_LONG_VARCHAR:
                case DPI_ORACLE_TYPE_LONG_NVARCHAR:
                case DPI_ORACLE_TYPE_LONG_RAW:
                    if (var->dynamicBytes)
                        return dpiVar__setBytesFromDynamicBytes(var, bytes,
                                &var->dynamicBytes[pos], error);
                    bytes->length = var->actualLength[pos];
                    return DPI_SUCCESS;
                case DPI_ORACLE_TYPE_CLOB:
                case DPI_ORACLE_TYPE_NCLOB:
                case DPI_ORACLE_TYPE_BLOB:
                case DPI_ORACLE_TYPE_BFILE:
                    return dpiVar__setBytesFromLob(var, bytes,
                            &var->dynamicBytes[pos],
                            var->references[pos].asLOB, error);
                case DPI_ORACLE_TYPE_NUMBER:
                    return dpiData__fromOracleNumberAsText(data, var, pos,
                            error, &var->data.asNumber[pos]);
                default:
                    break;
            }
            break;
        case DPI_NATIVE_TYPE_FLOAT:
            data->value.asFloat = var->data.asFloat[pos];
            break;
        case DPI_NATIVE_TYPE_TIMESTAMP:
            if (oracleTypeNum == DPI_ORACLE_TYPE_DATE)
                return dpiData__fromOracleDate(data, &var->data.asDate[pos]);
            return dpiData__fromOracleTimestamp(data, var->env, error,
                    var->data.asTimestamp[pos],
                    oracleTypeNum != DPI_ORACLE_TYPE_TIMESTAMP);
            break;
        case DPI_NATIVE_TYPE_INTERVAL_DS:
            return dpiData__fromOracleIntervalDS(data, var->env, error,
                    var->data.asInterval[pos]);
        case DPI_NATIVE_TYPE_INTERVAL_YM:
            return dpiData__fromOracleIntervalYM(data, var->env, error,
                    var->data.asInterval[pos]);
        case DPI_NATIVE_TYPE_OBJECT:
            data->value.asObject = NULL;
            if (!var->references[pos].asObject) {
                if (dpiObject__allocate(var->objectType,
                        var->data.asObject[pos], var->objectIndicator[pos], 1,
                        &var->references[pos].asObject, error) < 0)
                    return DPI_FAILURE;
            }
            data->value.asObject = var->references[pos].asObject;
            break;
        case DPI_NATIVE_TYPE_STMT:
            data->value.asStmt = var->references[pos].asStmt;
            break;
        case DPI_NATIVE_TYPE_BOOLEAN:
            data->value.asBoolean = var->data.asBoolean[pos];
            break;
        default:
            break;
    }
    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiVar__inBindCallback() [INTERNAL]
//   Callback which runs during OCI statement execution and provides buffers to
// OCI for binding data IN. This is not used with DML returning so this method
// does nothing useful except satisfy OCI requirements.
//-----------------------------------------------------------------------------
int32_t dpiVar__inBindCallback(dpiVar *var, OCIBind *bindp, uint32_t iter,
        uint32_t index, void **bufpp, uint32_t *alenp, uint8_t *piecep,
        void **indpp)
{
    dpiDynamicBytes *dynBytes;

    if (var->isDynamic) {
        dynBytes = &var->dynamicBytes[index];
        if (dynBytes->allocatedChunks == 0) {
            *bufpp = NULL;
            *alenp = 0;
        } else {
            *bufpp = dynBytes->chunks->ptr;
            *alenp = dynBytes->chunks->length;
        }
    } else {
        dpiVar__assignCallbackBuffer(var, index, bufpp);
        *alenp = (var->actualLength) ? var->actualLength[index] :
                var->type->sizeInBytes;
    }
    *piecep = OCI_ONE_PIECE;
    *indpp = &var->indicator[index];
    return OCI_CONTINUE;
}


//-----------------------------------------------------------------------------
// dpiVar__initBuffers() [INTERNAL]
//   Initialize buffers necessary for passing data to/from Oracle.
//-----------------------------------------------------------------------------
static int dpiVar__initBuffers(dpiVar *var, dpiError *error)
{
    if (dpiVar__allocateBuffers(var, error) < 0)
        return DPI_FAILURE;
    return dpiVar__extendedInitialize(var, error);
}


//-----------------------------------------------------------------------------
// dpiVar__outBindCallback() [INTERNAL]
//   Callback which runs during OCI statement execution and allocates the
// buffers required as well as provides that information to the OCI. This is
// intended for use with DML returning only.
//-----------------------------------------------------------------------------
int32_t dpiVar__outBindCallback(dpiVar *var, OCIBind *bindp, uint32_t iter,
        uint32_t index, void **bufpp, uint32_t **alenpp, uint8_t *piecep,
        void **indpp, uint16_t **rcodepp)
{
    uint32_t numRowsReturned;
    sword status;

    // special processing during first iteration
    if (index == 0) {

        // determine number of rows returned
        status = OCIAttrGet(bindp, OCI_HTYPE_BIND, &numRowsReturned, 0,
                OCI_ATTR_ROWS_RETURNED, var->error->handle);
        if (dpiError__check(var->error, status, var->conn,
                "get rows returned") < 0)
            return OCI_ERROR;

        // reallocate buffers, if needed
        if (numRowsReturned > var->maxArraySize) {
            dpiVar__finalizeBuffers(var, var->error);
            var->maxArraySize = numRowsReturned;
            if (dpiVar__initBuffers(var, var->error) < 0)
                return OCI_ERROR;
        }

    }

    // assign pointers used by OCI
    *piecep = OCI_ONE_PIECE;
    dpiVar__assignCallbackBuffer(var, index, bufpp);
    if (var->actualLength) {
#if DPI_ORACLE_CLIENT_VERSION_HEX < DPI_ORACLE_CLIENT_VERSION(12,1)
        if (!var->dynamicActualLength) {
            var->dynamicActualLength = calloc(var->maxArraySize,
                    sizeof(uint32_t));
            if (!var->dynamicActualLength) {
                dpiError__set(var->error, "allocate lengths for 11g",
                        DPI_ERR_NO_MEMORY);
                return OCI_ERROR;
            }
        }
        var->dynamicActualLength[index] = var->sizeInBytes;
        *alenpp = &(var->dynamicActualLength[index]);
#else
        var->actualLength[index] = var->sizeInBytes;
        *alenpp = &(var->actualLength[index]);
#endif
    } else if (*alenpp && var->type->sizeInBytes)
        **alenpp = var->type->sizeInBytes;
    *indpp = &(var->indicator[index]);
    if (var->returnCode)
        *rcodepp = &var->returnCode[index];

    return OCI_CONTINUE;
}


//-----------------------------------------------------------------------------
// dpiVar__setBytesFromDynamicBytes() [PRIVATE]
//   Set the pointer and length in the dpiBytes structure to the values
// retrieved from the database. At this point, if multiple chunks exist, they
// are combined into one.
//-----------------------------------------------------------------------------
static int dpiVar__setBytesFromDynamicBytes(dpiVar *var, dpiBytes *bytes,
        dpiDynamicBytes *dynBytes, dpiError *error)
{
    uint32_t i, totalAllocatedLength;

    // if only one chunk is available, make use of it
    if (dynBytes->numChunks == 1) {
        bytes->ptr = dynBytes->chunks->ptr;
        bytes->length = dynBytes->chunks->length;
        return DPI_SUCCESS;
    }

    // determine total allocated size of all chunks
    totalAllocatedLength = 0;
    for (i = 0; i < dynBytes->numChunks; i++)
        totalAllocatedLength += dynBytes->chunks[i].allocatedLength;

    // allocate new memory consolidating all of the chunks
    bytes->ptr = malloc(totalAllocatedLength);
    if (!bytes->ptr)
        return dpiError__set(error, "allocate chunk", DPI_ERR_NO_MEMORY);

    // copy memory from chunks to consolidated chunk
    bytes->length = 0;
    for (i = 0; i < dynBytes->numChunks; i++) {
        memcpy(bytes->ptr + bytes->length, dynBytes->chunks[i].ptr,
                dynBytes->chunks[i].length);
        bytes->length += dynBytes->chunks[i].length;
        free(dynBytes->chunks[i].ptr);
        dynBytes->chunks[i].ptr = NULL;
        dynBytes->chunks[i].length = 0;
        dynBytes->chunks[i].allocatedLength = 0;
    }

    // populate first chunk with consolidated information
    dynBytes->numChunks = 1;
    dynBytes->chunks->ptr = bytes->ptr;
    dynBytes->chunks->length = bytes->length;
    dynBytes->chunks->allocatedLength = totalAllocatedLength;

    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiVar__setBytesFromLob() [PRIVATE]
//   Populate the dynamic bytes structure with the data from the LOB and then
// populate the bytes structure.
//-----------------------------------------------------------------------------
static int dpiVar__setBytesFromLob(dpiVar *var, dpiBytes *bytes,
        dpiDynamicBytes *dynBytes, dpiLob *lob, dpiError *error)
{
    uint64_t length, lengthInBytes, lengthReadInBytes;
    sword status;

    // determine length of LOB in bytes
    status = OCILobGetLength2(lob->conn->handle, error->handle, lob->locator,
            (ub8*) &length);
    if (dpiError__check(error, status, lob->conn, "get LOB length") < 0)
        return DPI_FAILURE;
    if (lob->type->oracleTypeNum == DPI_ORACLE_TYPE_CLOB)
        lengthInBytes = length * lob->env->maxBytesPerCharacter;
    else if (lob->type->oracleTypeNum == DPI_ORACLE_TYPE_NCLOB)
        lengthInBytes = length * lob->env->nmaxBytesPerCharacter;
    else lengthInBytes = length;

    // ensure there is enough space to store the entire LOB value
    if (lengthInBytes > UB4MAXVAL)
        return dpiError__set(error, "check max length", DPI_ERR_NOT_SUPPORTED);
    if (dpiVar__allocateDynamicBytes(dynBytes, (uint32_t) lengthInBytes,
            error) < 0)
        return DPI_FAILURE;

    // read data from the LOB
    lengthReadInBytes = lengthInBytes;
    if (length > 0 && dpiLob__readBytes(lob, 1, length, dynBytes->chunks->ptr,
            &lengthReadInBytes, error) < 0)
        return DPI_FAILURE;

    dynBytes->chunks->length = (uint32_t) lengthReadInBytes;
    bytes->ptr = dynBytes->chunks->ptr;
    bytes->length = dynBytes->chunks->length;
    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiVar__setFromBytes() [PRIVATE]
//   Set the value of the variable at the given array position from a byte
// string. The byte string is not retained in any way. A copy will be made into
// buffers allocated by ODPI-C.
//-----------------------------------------------------------------------------
static int dpiVar__setFromBytes(dpiVar *var, uint32_t pos, const char *value,
        uint32_t valueLength, dpiError *error)
{
    dpiDynamicBytes *dynBytes;
    dpiBytes *bytes;
    dpiData *data;

    // validate the target can accept the input
    if ((var->tempBuffer && var->env->charsetId == DPI_CHARSET_ID_UTF16 &&
                    valueLength > DPI_NUMBER_AS_TEXT_CHARS * 2) ||
            (var->tempBuffer && var->env->charsetId != DPI_CHARSET_ID_UTF16 &&
                    valueLength > DPI_NUMBER_AS_TEXT_CHARS) ||
            (!var->dynamicBytes && !var->tempBuffer &&
                    valueLength > var->sizeInBytes))
        return dpiError__set(error, "check source length",
                DPI_ERR_BUFFER_SIZE_TOO_SMALL, var->sizeInBytes);

    // mark the value as not null
    data = &var->externalData[pos];
    data->isNull = 0;

    // for internally used LOBs, write the data directly
    if (var->references)
        return dpiLob__setFromBytes(var->references[pos].asLOB, value,
                valueLength, error);

    // for dynamic bytes, allocate space as needed
    bytes = &data->value.asBytes;
    if (var->dynamicBytes) {
        dynBytes = &var->dynamicBytes[pos];
        if (dpiVar__allocateDynamicBytes(dynBytes, valueLength, error) < 0)
            return DPI_FAILURE;
        memcpy(dynBytes->chunks->ptr, value, valueLength);
        dynBytes->numChunks = 1;
        dynBytes->chunks->length = valueLength;
        bytes->ptr = dynBytes->chunks->ptr;
        bytes->length = valueLength;

    // for everything else, space has already been allocated
    } else {
        bytes->length = valueLength;
        if (valueLength > 0)
            memcpy(bytes->ptr, value, valueLength);
        if (var->actualLength)
            var->actualLength[pos] = (DPI_ACTUAL_LENGTH_TYPE) valueLength;
        if (var->returnCode)
            var->returnCode[pos] = 0;
    }

    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiVar__setFromLob() [PRIVATE]
//   Set the value of the variable at the given array position from a LOB.
// A reference to the LOB is retained by the variable.
//-----------------------------------------------------------------------------
static int dpiVar__setFromLob(dpiVar *var, uint32_t pos, dpiLob *lob,
        dpiError *error)
{
    dpiData *data;

    // validate the LOB object
    if (dpiGen__checkHandle(lob, DPI_HTYPE_LOB, "check LOB", error) < 0)
        return DPI_FAILURE;

    // mark the value as not null
    data = &var->externalData[pos];
    data->isNull = 0;

    // if values are the same, nothing to do
    if (var->references[pos].asLOB == lob)
        return DPI_SUCCESS;

    // clear original value, if needed
    if (var->references[pos].asLOB) {
        dpiGen__setRefCount(var->references[pos].asLOB, error, -1);
        var->references[pos].asLOB = NULL;
    }

    // add reference to passed object
    dpiGen__setRefCount(lob, error, 1);
    var->references[pos].asLOB = lob;
    var->data.asLobLocator[pos] = lob->locator;
    data->value.asLOB = lob;
    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiVar__setFromObject() [PRIVATE]
//   Set the value of the variable at the given array position from an object.
// The variable and position are assumed to be valid at this point. A reference
// to the object is retained by the variable.
//-----------------------------------------------------------------------------
static int dpiVar__setFromObject(dpiVar *var, uint32_t pos, dpiObject *obj,
        dpiError *error)
{
    dpiData *data;

    // validate the object
    if (dpiGen__checkHandle(obj, DPI_HTYPE_OBJECT, "check obj", error) < 0)
        return DPI_FAILURE;

    // mark the value as not null
    data = &var->externalData[pos];
    data->isNull = 0;

    // if values are the same, nothing to do
    if (var->references[pos].asObject == obj)
        return DPI_SUCCESS;

    // clear original value, if needed
    if (var->references[pos].asObject) {
        dpiGen__setRefCount(var->references[pos].asObject, error, -1);
        var->references[pos].asObject = NULL;
    }

    // add reference to passed object
    dpiGen__setRefCount(obj, error, 1);
    var->references[pos].asObject = obj;
    var->data.asObject[pos] = obj->instance;
    var->objectIndicator[pos] = obj->indicator;
    data->value.asObject = obj;
    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiVar__setFromRowid() [PRIVATE]
//   Set the value of the variable at the given array position from a rowid.
// A reference to the rowid is retained by the variable.
//-----------------------------------------------------------------------------
static int dpiVar__setFromRowid(dpiVar *var, uint32_t pos, dpiRowid *rowid,
        dpiError *error)
{
    dpiData *data;

    // validate the rowid
    if (dpiGen__checkHandle(rowid, DPI_HTYPE_ROWID, "check rowid", error) < 0)
        return DPI_FAILURE;

    // mark the value as not null
    data = &var->externalData[pos];
    data->isNull = 0;

    // if values are the same, nothing to do
    if (var->references[pos].asRowid == rowid)
        return DPI_SUCCESS;

    // clear original value, if needed
    if (var->references[pos].asRowid) {
        dpiGen__setRefCount(var->references[pos].asRowid, error, -1);
        var->references[pos].asRowid = NULL;
    }

    // add reference to passed object
    dpiGen__setRefCount(rowid, error, 1);
    var->references[pos].asRowid = rowid;
    var->data.asRowid[pos] = rowid->handle;
    data->value.asRowid = rowid;
    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiVar__setFromStmt() [PRIVATE]
//   Set the value of the variable at the given array position from a
// statement. A reference to the statement is retained by the variable.
//-----------------------------------------------------------------------------
static int dpiVar__setFromStmt(dpiVar *var, uint32_t pos, dpiStmt *stmt,
        dpiError *error)
{
    dpiData *data;

    // validate the statement
    if (dpiGen__checkHandle(stmt, DPI_HTYPE_STMT, "check stmt", error) < 0)
        return DPI_FAILURE;

    // mark the value as not null
    data = &var->externalData[pos];
    data->isNull = 0;

    // if values are the same, nothing to do
    if (var->references[pos].asStmt == stmt)
        return DPI_SUCCESS;

    // clear original value, if needed
    if (var->references[pos].asStmt) {
        dpiGen__setRefCount(var->references[pos].asStmt, error, -1);
        var->references[pos].asStmt = NULL;
    }

    // add reference to passed object
    dpiGen__setRefCount(stmt, error, 1);
    var->references[pos].asStmt = stmt;
    var->data.asStmt[pos] = stmt->handle;
    data->value.asStmt = stmt;
    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiVar__setValue() [PRIVATE]
//   Sets the contents of the variable using the type specified, if possible.
//-----------------------------------------------------------------------------
int dpiVar__setValue(dpiVar *var, uint32_t pos, dpiData *data,
        dpiError *error)
{
    dpiOracleTypeNum oracleTypeNum;
    dpiObject *obj;

    // if value is null, no need to proceed further
    // however, when binding objects a value MUST be present or OCI will
    // segfault!
    if (data->isNull) {
        var->indicator[pos] = OCI_IND_NULL;
        if (var->objectIndicator && !var->data.asObject[pos]) {
            if (dpiObjectType_createObject(var->objectType, &obj) < 0)
                return DPI_FAILURE;
            var->references[pos].asObject = obj;
            data->value.asObject = obj;
            var->data.asObject[pos] = obj->instance;
            var->objectIndicator[pos] = obj->indicator;
            if (var->objectIndicator[pos])
                *((OCIInd*) var->objectIndicator[pos]) = OCI_IND_NULL;
        }
        return DPI_SUCCESS;
    }

    // transform the various types
    var->indicator[pos] = OCI_IND_NOTNULL;
    oracleTypeNum = var->type->oracleTypeNum;
    switch (var->nativeTypeNum) {
        case DPI_NATIVE_TYPE_INT64:
        case DPI_NATIVE_TYPE_UINT64:
            switch (oracleTypeNum) {
                case DPI_ORACLE_TYPE_NATIVE_INT:
                    var->data.asInt64[pos] = data->value.asInt64;
                    return DPI_SUCCESS;
                case DPI_ORACLE_TYPE_NUMBER:
                    return dpiData__toOracleNumberFromInteger(data, var->env,
                            error, &var->data.asNumber[pos]);
                default:
                    break;
            }
            break;
        case DPI_NATIVE_TYPE_FLOAT:
            var->data.asFloat[pos] = data->value.asFloat;
            return DPI_SUCCESS;
        case DPI_NATIVE_TYPE_DOUBLE:
            switch (oracleTypeNum) {
                case DPI_ORACLE_TYPE_NATIVE_DOUBLE:
                    var->data.asDouble[pos] = data->value.asDouble;
                    return DPI_SUCCESS;
                case DPI_ORACLE_TYPE_NUMBER:
                    return dpiData__toOracleNumberFromDouble(data, var->env,
                            error, &var->data.asNumber[pos]);
                case DPI_ORACLE_TYPE_TIMESTAMP:
                case DPI_ORACLE_TYPE_TIMESTAMP_TZ:
                case DPI_ORACLE_TYPE_TIMESTAMP_LTZ:
                    return dpiData__toOracleTimestampFromDouble(data, var->env,
                            error, var->data.asTimestamp[pos]);
                default:
                    break;
            }
            break;
        case DPI_NATIVE_TYPE_BYTES:
            if (oracleTypeNum == DPI_ORACLE_TYPE_NUMBER)
                return dpiData__toOracleNumberFromText(data, var->env,
                        error, &var->data.asNumber[pos]);
            if (var->returnCode)
                var->returnCode[pos] = 0;
            break;
        case DPI_NATIVE_TYPE_TIMESTAMP:
            if (oracleTypeNum == DPI_ORACLE_TYPE_DATE)
                return dpiData__toOracleDate(data, &var->data.asDate[pos]);
            else if (oracleTypeNum == DPI_ORACLE_TYPE_TIMESTAMP)
                return dpiData__toOracleTimestamp(data, var->env, error,
                        var->data.asTimestamp[pos], 0);
            else if (oracleTypeNum == DPI_ORACLE_TYPE_TIMESTAMP_TZ ||
                    oracleTypeNum == DPI_ORACLE_TYPE_TIMESTAMP_LTZ)
                return dpiData__toOracleTimestamp(data, var->env, error,
                        var->data.asTimestamp[pos], 1);
            break;
        case DPI_NATIVE_TYPE_INTERVAL_DS:
            return dpiData__toOracleIntervalDS(data, var->env, error,
                    var->data.asInterval[pos]);
        case DPI_NATIVE_TYPE_INTERVAL_YM:
            return dpiData__toOracleIntervalYM(data, var->env, error,
                    var->data.asInterval[pos]);
        case DPI_NATIVE_TYPE_BOOLEAN:
            var->data.asBoolean[pos] = data->value.asBoolean;
            return DPI_SUCCESS;
        default:
            break;
    }
    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiVar__validateTypes() [PRIVATE]
//   Validate that the Oracle type and the native type are compatible with
// each other when the native type is not already the default native type.
//-----------------------------------------------------------------------------
static int dpiVar__validateTypes(const dpiOracleType *oracleType,
        dpiNativeTypeNum nativeTypeNum, dpiError *error)
{
    switch (oracleType->oracleTypeNum) {
        case DPI_ORACLE_TYPE_TIMESTAMP:
        case DPI_ORACLE_TYPE_TIMESTAMP_TZ:
        case DPI_ORACLE_TYPE_TIMESTAMP_LTZ:
            if (nativeTypeNum == DPI_NATIVE_TYPE_DOUBLE)
                return DPI_SUCCESS;
            break;
        case DPI_ORACLE_TYPE_NATIVE_INT:
            if (nativeTypeNum == DPI_NATIVE_TYPE_UINT64)
                return DPI_SUCCESS;
            break;
        case DPI_ORACLE_TYPE_NUMBER:
            if (nativeTypeNum == DPI_NATIVE_TYPE_INT64 ||
                    nativeTypeNum == DPI_NATIVE_TYPE_UINT64 ||
                    nativeTypeNum == DPI_NATIVE_TYPE_BYTES)
                return DPI_SUCCESS;
            break;
        default:
            break;
    }
    return dpiError__set(error, "validate types", DPI_ERR_UNHANDLED_CONVERSION,
            oracleType->oracleTypeNum, nativeTypeNum);
}


//-----------------------------------------------------------------------------
// dpiVar_addRef() [PUBLIC]
//   Add a reference to the variable.
//-----------------------------------------------------------------------------
int dpiVar_addRef(dpiVar *var)
{
    return dpiGen__addRef(var, DPI_HTYPE_VAR, __func__);
}


//-----------------------------------------------------------------------------
// dpiVar_copyData() [PUBLIC]
//   Copy the data from the source variable to the target variable at the given
// array position. The variables must use the same native type. If the
// variables contain variable length data, the source length must not exceed
// the target allocated memory.
//-----------------------------------------------------------------------------
int dpiVar_copyData(dpiVar *var, uint32_t pos, dpiVar *sourceVar,
        uint32_t sourcePos)
{
    dpiData *sourceData;
    dpiError error;

    if (dpiVar__checkArraySize(var, pos, __func__, &error) < 0)
        return DPI_FAILURE;
    if (dpiGen__checkHandle(sourceVar, DPI_HTYPE_VAR, "check source var",
            &error) < 0)
        return DPI_FAILURE;
    if (sourcePos >= sourceVar->maxArraySize)
        return dpiError__set(&error, "check source size",
                DPI_ERR_ARRAY_SIZE_EXCEEDED, sourceVar->maxArraySize,
                sourcePos);
    if (var->nativeTypeNum != sourceVar->nativeTypeNum)
        return dpiError__set(&error, "check types match",
                DPI_ERR_NOT_SUPPORTED);
    sourceData = &sourceVar->externalData[sourcePos];
    return dpiVar__copyData(var, pos, sourceData, &error);
}


//-----------------------------------------------------------------------------
// dpiVar_getData() [PUBLIC]
//   Return a pointer to the array of dpiData structures allocated for the
// variable and the number of elements. These structures are used for
// transferring data and are populated after an internal execute or fetch is
// performed (out variables) and before an internal execute is performed (in
// variables). This routine is needed for DML returning where the number of
// elements and the external data structure is modified during execution; in
// all other cases the values returned when the variable is allocated will not
// change.
//-----------------------------------------------------------------------------
int dpiVar_getData(dpiVar *var, uint32_t *numElements, dpiData **data)
{
    dpiError error;

    if (dpiGen__startPublicFn(var, DPI_HTYPE_VAR, __func__, &error) < 0)
        return DPI_FAILURE;
    *numElements = var->maxArraySize;
    *data = var->externalData;
    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiVar_getNumElementsInArray() [PUBLIC]
//   Return the actual number of elements in the array. This value is only
// relevant if the variable is bound as an array.
//-----------------------------------------------------------------------------
int dpiVar_getNumElementsInArray(dpiVar *var, uint32_t *numElements)
{
    dpiError error;

    if (dpiGen__startPublicFn(var, DPI_HTYPE_VAR, __func__, &error) < 0)
        return DPI_FAILURE;
    *numElements = var->actualArraySize;
    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiVar_getSizeInBytes() [PUBLIC]
//   Returns the size in bytes of the buffer allocated for the variable.
//-----------------------------------------------------------------------------
int dpiVar_getSizeInBytes(dpiVar *var, uint32_t *sizeInBytes)
{
    dpiError error;

    if (dpiGen__startPublicFn(var, DPI_HTYPE_VAR, __func__, &error) < 0)
        return DPI_FAILURE;
    *sizeInBytes = var->sizeInBytes;
    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiVar_release() [PUBLIC]
//   Release a reference to the variable.
//-----------------------------------------------------------------------------
int dpiVar_release(dpiVar *var)
{
    return dpiGen__release(var, DPI_HTYPE_VAR, __func__);
}


//-----------------------------------------------------------------------------
// dpiVar_resize() [PUBLIC]
//   Resize the buffer allocated for the variable to the given size.
//-----------------------------------------------------------------------------
int dpiVar_resize(dpiVar *var, uint32_t sizeInBytes)
{
    dpiError error;

    if (dpiGen__startPublicFn(var, DPI_HTYPE_VAR, __func__, &error) < 0)
        return DPI_FAILURE;
    if (var->nativeTypeNum != DPI_NATIVE_TYPE_BYTES)
        return dpiError__set(&error, "resize", DPI_ERR_NOT_SUPPORTED);
    if (var->dynamicBytes)
        return DPI_SUCCESS;
    free(var->data.asRaw);
    var->data.asRaw = NULL;
    var->sizeInBytes = sizeInBytes;
    return dpiVar__allocateBuffers(var, &error);
}


//-----------------------------------------------------------------------------
// dpiVar_setFromBytes() [PUBLIC]
//   Set the value of the variable at the given array position from a byte
// string. Checks on the array position, the size of the string and the type of
// variable will be made. The byte string is not retained in any way. A copy
// will be made into buffers allocated by ODPI-C.
//-----------------------------------------------------------------------------
int dpiVar_setFromBytes(dpiVar *var, uint32_t pos, const char *value,
        uint32_t valueLength)
{
    dpiError error;

    // validate the inputs
    if (dpiVar__checkArraySize(var, pos, __func__, &error) < 0)
        return DPI_FAILURE;
    if (var->nativeTypeNum != DPI_NATIVE_TYPE_BYTES)
        return dpiError__set(&error, "native type", DPI_ERR_NOT_SUPPORTED);
    return dpiVar__setFromBytes(var, pos, value, valueLength, &error);
}


//-----------------------------------------------------------------------------
// dpiVar_setFromLob() [PUBLIC]
//   Set the value of the variable at the given array position from a LOB.
// Checks on the array position and the validity of the passed handle. A
// reference to the LOB is retained by the variable.
//-----------------------------------------------------------------------------
int dpiVar_setFromLob(dpiVar *var, uint32_t pos, dpiLob *lob)
{
    dpiError error;

    // validate the inputs
    if (dpiVar__checkArraySize(var, pos, __func__, &error) < 0)
        return DPI_FAILURE;
    if (var->nativeTypeNum != DPI_NATIVE_TYPE_LOB)
        return dpiError__set(&error, "native type", DPI_ERR_NOT_SUPPORTED);
    return dpiVar__setFromLob(var, pos, lob, &error);
}


//-----------------------------------------------------------------------------
// dpiVar_setFromObject() [PUBLIC]
//   Set the value of the variable at the given array position from an object.
// Checks on the array position and the validity of the passed handle. A
// reference to the object is retained by the variable.
//-----------------------------------------------------------------------------
int dpiVar_setFromObject(dpiVar *var, uint32_t pos, dpiObject *obj)
{
    dpiError error;

    if (dpiVar__checkArraySize(var, pos, __func__, &error) < 0)
        return DPI_FAILURE;
    if (var->nativeTypeNum != DPI_NATIVE_TYPE_OBJECT)
        return dpiError__set(&error, "native type", DPI_ERR_NOT_SUPPORTED);
    return dpiVar__setFromObject(var, pos, obj, &error);
}


//-----------------------------------------------------------------------------
// dpiVar_setFromRowid() [PUBLIC]
//   Set the value of the variable at the given array position from a rowid.
// Checks on the array position and the validity of the passed handle. A
// reference to the rowid is retained by the variable.
//-----------------------------------------------------------------------------
int dpiVar_setFromRowid(dpiVar *var, uint32_t pos, dpiRowid *rowid)
{
    dpiError error;

    if (dpiVar__checkArraySize(var, pos, __func__, &error) < 0)
        return DPI_FAILURE;
    if (var->nativeTypeNum != DPI_NATIVE_TYPE_ROWID)
        return dpiError__set(&error, "native type", DPI_ERR_NOT_SUPPORTED);
    return dpiVar__setFromRowid(var, pos, rowid, &error);
}


//-----------------------------------------------------------------------------
// dpiVar_setFromStmt() [PUBLIC]
//   Set the value of the variable at the given array position from a
// statement. Checks on the array position and the validity of the passed
// handle. A reference to the statement is retained by the variable.
//-----------------------------------------------------------------------------
int dpiVar_setFromStmt(dpiVar *var, uint32_t pos, dpiStmt *stmt)
{
    dpiError error;

    if (dpiVar__checkArraySize(var, pos, __func__, &error) < 0)
        return DPI_FAILURE;
    if (var->nativeTypeNum != DPI_NATIVE_TYPE_STMT)
        return dpiError__set(&error, "native type", DPI_ERR_NOT_SUPPORTED);
    return dpiVar__setFromStmt(var, pos, stmt, &error);
}


//-----------------------------------------------------------------------------
// dpiVar_setNumElementsInArray() [PUBLIC]
//   Set the number of elements in the array (different from the number of
// allocated elements).
//-----------------------------------------------------------------------------
int dpiVar_setNumElementsInArray(dpiVar *var, uint32_t numElements)
{
    dpiError error;

    if (dpiGen__startPublicFn(var, DPI_HTYPE_VAR, __func__, &error) < 0)
        return DPI_FAILURE;
    if (numElements > var->maxArraySize)
        return dpiError__set(&error, "check num elements",
                DPI_ERR_ARRAY_SIZE_EXCEEDED, var->maxArraySize, numElements);
    var->actualArraySize = numElements;
    return DPI_SUCCESS;
}


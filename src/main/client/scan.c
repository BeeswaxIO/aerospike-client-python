/*******************************************************************************
 * Copyright 2013-2014 Aerospike, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ******************************************************************************/

#include <Python.h>
#include <stdbool.h>

#include "client.h"
#include "scan.h"
#include "policy.h"
#include "conversions.h"
#include <aerospike/aerospike_scan.h>
#include <aerospike/as_arraylist.h>
#include <aerospike/as_scan.h>

#define PROGRESS_PCT "progress_pct"
#define RECORDS_SCANNED "records_scanned"
#define STATUS "status"

AerospikeScan * AerospikeClient_Scan(AerospikeClient * self, PyObject * args, PyObject * kwds)
{
    return AerospikeScan_New(self, args, kwds);
}

/**
 * Scans a set in the Aerospike DB and applies UDF on it.
 *
 * @param self                  The c client's aerospike object.
 * @param namespace_p           The namespace to scan.
 * @param set_p                 The set to scan.
 * @param module_p              The name of UDF module containing the
 *                              function to execute.
 * @param function_p            The name of the function to be applied
 *                              to the record.
 * @param py_args               An array of arguments for the UDF.
 * @py_policy                   The optional policy.
 * @py_options                  The optional scan options to set.
 */
static
PyObject * AerospikeClient_ScanApply_Invoke(
	AerospikeClient * self, 
	char* namespace_p, char* set_p, char* module_p, char* function_p,
    PyObject * py_args, PyObject * py_policy, PyObject * py_options)
{
    as_list* arglist = NULL;
    as_policy_scan scan_policy;
    as_policy_scan* scan_policy_p = NULL;
    as_error err;
    as_scan scan;
    uint64_t scan_id = 0;
    bool is_scan_init = false;

	// Initialize error
	as_error_init(&err);

    if (!self || !self->as) {
		as_error_update(&err, AEROSPIKE_ERR_PARAM, "Invalid aerospike object");
        goto CLEANUP;
    }

    if (!(namespace_p) || !(set_p) || !(module_p) || !(function_p)) {
		as_error_update(&err, AEROSPIKE_ERR_PARAM, "Parameter should not be null");
        goto CLEANUP;
    }

    if (!PyList_Check(py_args)) {
		as_error_update(&err, AEROSPIKE_ERR_PARAM, "Arguments should be a list");
        goto CLEANUP;
    }

    as_scan_init(&scan, namespace_p, set_p);
    is_scan_init = true;

    if (py_policy) {
        validate_policy_scan(&err, py_policy, &scan_policy);

        if (err.code != AEROSPIKE_OK) {
            goto CLEANUP;
        }

        pyobject_to_policy_scan(&err, py_policy, &scan_policy, &scan_policy_p);

        if (err.code != AEROSPIKE_OK) {
            goto CLEANUP;
        }
    }

    if (py_options) {
        set_scan_options(&err, &scan, py_options);
    }

    if (err.code != AEROSPIKE_OK) {
        goto CLEANUP;
    }

    pyobject_to_list(&err, py_args, &arglist);
    if (err.code != AEROSPIKE_OK) {
        goto CLEANUP;
    }

    if (!as_scan_apply_each(&scan, module_p, function_p, arglist)) {
		as_error_update(&err, AEROSPIKE_ERR_PARAM, "Unable to apply UDF on the scan");
        goto CLEANUP;
    }

    aerospike_scan_background(self->as, &err, scan_policy_p, &scan, &scan_id);
    arglist = NULL;

CLEANUP:
    
    if (arglist) {
        as_list_destroy(arglist);
    }

    if (is_scan_init) {
        as_scan_destroy(&scan);
    }

	if ( err.code != AEROSPIKE_OK ) {
		PyObject * py_err = NULL;
		error_to_pyobject(&err, &py_err);
		PyErr_SetObject(PyExc_Exception, py_err);
	    Py_DECREF(py_err);
        return NULL;
	}
	
	return PyLong_FromLong(scan_id);
}

/**
 ******************************************************************************************************
 * Apply a record UDF to each record in a background scan.
 * 
 * @param self                  AerospikeClient object
 * @param args                  The args is a tuple object containing an argument
 *                              list passed from Python to a C function
 * @param kwds                  Dictionary of keywords
 *
 * Returns  integer handle for the initiated background scan.
 *******************************************************************************************************
 */
PyObject * AerospikeClient_ScanApply(AerospikeClient * self, PyObject * args, PyObject * kwds)
{
	// Python Function Arguments
	PyObject * py_args = NULL;
	PyObject * py_policy = NULL;
	PyObject * py_options = NULL;

	// Python Function Keyword Arguments
	static char * kwlist[] = {"ns", "set", "module", "function", "args", "policy", "options", NULL};
    char *namespace = NULL, *set = NULL, *module = NULL, *function = NULL;

	// Python Function Argument Parsing
	if ( PyArg_ParseTupleAndKeywords(args, kwds, "ssssO|OO:scan_apply", kwlist, &namespace, &set,
                &module, &function, &py_args, &py_policy, &py_options) == false ) {
		return NULL;
	}

	// Invoke Operation
	return AerospikeClient_ScanApply_Invoke(self, namespace, set, module,
            function, py_args, py_policy, py_options);
}

/**
 *******************************************************************************************************
 * Gets the status of a background scan triggered by scanApply()
 * 
 * @param self                  AerospikeClient object
 * @param args                  The args is a tuple object containing an argument
 *                              list passed from Python to a C function
 * @param kwds                  Dictionary of keywords
 *
 * Returns status of the background scan returned as a tuple containing
 * progress_pct, records_scanned, status.
 *******************************************************************************************************
 */
PyObject * AerospikeClient_ScanInfo(AerospikeClient * self, PyObject * args, PyObject * kwds)
{
    // Initialize error
    as_error err;
    as_error_init(&err);

    // Python Function Arguments
    PyObject * py_policy = NULL;
    PyObject * retObj = PyDict_New();

    long lscanId = 0;

    as_policy_info info_policy;
    as_policy_info *info_policy_p = NULL;
    as_scan_info scan_info;

    // Python Function Keyword Arguments
    static char * kwlist[] = {"scanid", "policy", NULL};

    // Python Function Argument Parsing
    if ( PyArg_ParseTupleAndKeywords(args, kwds, "l|O:scan_info", kwlist, &lscanId, &py_policy) == false ) {
        return NULL;
    }

    if (!self || !self->as) {
		as_error_update(&err, AEROSPIKE_ERR_PARAM, "Invalid aerospike object");
        goto CLEANUP;
    }

    if (py_policy) {
        validate_policy_scan(&err, py_policy, &info_policy);
    }
    if (err.code != AEROSPIKE_OK) {
        goto CLEANUP;
    }
    // Convert python object to policy_info 
    pyobject_to_policy_info( &err, py_policy, &info_policy, &info_policy_p );
    if ( err.code != AEROSPIKE_OK ) {
        goto CLEANUP;
    }

    if (AEROSPIKE_OK != (aerospike_scan_info(self->as, &err,
                    info_policy_p, lscanId, &scan_info))) {
        goto CLEANUP;
    }

    if(retObj)
    {
        PyObject * py_longobject = NULL;
        py_longobject = PyLong_FromLong(scan_info.progress_pct);
        PyDict_SetItemString(retObj, PROGRESS_PCT, py_longobject );
        Py_DECREF(py_longobject);
        py_longobject = PyLong_FromLong(scan_info.records_scanned);
        PyDict_SetItemString(retObj, RECORDS_SCANNED, py_longobject );
        Py_DECREF(py_longobject);
        py_longobject = PyLong_FromLong(scan_info.status + AS_SCAN_STATUS);
        PyDict_SetItemString(retObj, STATUS, py_longobject );
        Py_DECREF(py_longobject);
    }

CLEANUP:

    if ( err.code != AEROSPIKE_OK ) {
        PyObject * py_err = NULL;
        error_to_pyobject(&err, &py_err);
        PyErr_SetObject(PyExc_Exception, py_err);
        Py_DECREF(py_err);
        return NULL;
    }

    return retObj;

}

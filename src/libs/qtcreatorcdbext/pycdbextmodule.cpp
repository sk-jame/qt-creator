/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "pycdbextmodule.h"

#include "extensioncontext.h"
#include "symbolgroupvalue.h"
#include "stringutils.h"

#include "pyfield.h"
#include "pystdoutredirect.h"
#include "pytype.h"
#include "pyvalue.h"

#include <Python.h>
#include <structmember.h>

#include <iterator>

class CurrentSymbolGroup
{
public:
    IDebugSymbolGroup2 *get()
    {
        ULONG threadId = ExtensionCommandContext::instance()->threadId();
        CIDebugControl *control = ExtensionCommandContext::instance()->control();
        DEBUG_STACK_FRAME frame;
        if (FAILED(control->GetStackTrace(0, 0, 0, &frame, 1, NULL)))
            return nullptr;
        if (m_symbolGroup && m_threadId == threadId && m_frameNumber == frame.FrameNumber)
            return m_symbolGroup;
        return create(threadId, frame.FrameNumber);
    }

    IDebugSymbolGroup2 *create()
    {
        ULONG threadId = ExtensionCommandContext::instance()->threadId();
        CIDebugControl *control = ExtensionCommandContext::instance()->control();
        DEBUG_STACK_FRAME frame;
        if (FAILED(control->GetStackTrace(0, 0, 0, &frame, 1, NULL)))
            return nullptr;
        return create(threadId, frame.FrameNumber);
    }

private:
    IDebugSymbolGroup2 *create(ULONG threadId, ULONG64 frameNumber)
    {
        CIDebugSymbols *symbols = ExtensionCommandContext::instance()->symbols();
        if (FAILED(symbols->GetScopeSymbolGroup2(DEBUG_SCOPE_GROUP_ALL, NULL, &m_symbolGroup)))
            return nullptr;
        m_frameNumber = frameNumber;
        m_threadId = threadId;
        return m_symbolGroup;
    }

private:
    IDebugSymbolGroup2 *m_symbolGroup = nullptr;
    ULONG m_threadId = 0;
    ULONG64 m_frameNumber = 0;
};

static CurrentSymbolGroup currentSymbolGroup;

// cdbext python module
static PyObject *cdbext_parseAndEvaluate(PyObject *, PyObject *args) // -> Value
{
    char *expr;
    if (!PyArg_ParseTuple(args, "s", &expr))
        Py_RETURN_NONE;
    CIDebugControl *control = ExtensionCommandContext::instance()->control();
    control->SetExpressionSyntax(DEBUG_EXPR_CPLUSPLUS);
    DEBUG_VALUE value;
    if (FAILED(control->Evaluate(expr, DEBUG_VALUE_INT64, &value, NULL)))
        Py_RETURN_NONE;
    return Py_BuildValue("K", value.I64);
}

static PyObject *cdbext_lookupType(PyObject *, PyObject *args) // -> Type
{
    char *type;
    if (!PyArg_ParseTuple(args, "s", &type))
        Py_RETURN_NONE;
    return lookupType(type);
}

static PyObject *cdbext_listOfLocals(PyObject *, PyObject *args) // -> [ Value ]
{
    char *partialVariablesC;
    if (!PyArg_ParseTuple(args, "s", &partialVariablesC))
        Py_RETURN_NONE;

    const std::string partialVariable(partialVariablesC);
    IDebugSymbolGroup2 *symbolGroup = nullptr;
    auto locals = PyList_New(0);
    if (partialVariable.empty()) {
        symbolGroup = currentSymbolGroup.create();
        if (symbolGroup == nullptr)
            return locals;
    } else {
        symbolGroup = currentSymbolGroup.get();
        if (symbolGroup == nullptr)
            return locals;

        ULONG scopeEnd;
        if (FAILED(symbolGroup->GetNumberSymbols(&scopeEnd)))
            return locals;

        std::vector<std::string> inameTokens;
        split(partialVariable, '.', std::back_inserter(inameTokens));
        auto currentPartialIname = inameTokens.begin();
        ++currentPartialIname; // skip "local" part

        ULONG symbolGroupIndex = 0;
        for (;symbolGroupIndex < scopeEnd; ++symbolGroupIndex) {
            if (getSymbolName(symbolGroup, symbolGroupIndex) == *currentPartialIname) {
                PyList_Append(locals, createValue(symbolGroupIndex, symbolGroup));
                return locals;
            }
        }
    }

    ULONG symbolCount;
    if (FAILED(symbolGroup->GetNumberSymbols(&symbolCount)))
        return locals;
    for (ULONG index = 0; index < symbolCount; ++index)
        PyList_Append(locals, createValue(index, symbolGroup));
    return locals;
}

static PyObject *cdbext_pointerSize(PyObject *, PyObject *)
{
    return Py_BuildValue("i", pointerSize());
}

static PyObject *cdbext_readRawMemory(PyObject *, PyObject *args)
{
    ULONG64 address = 0;
    ULONG size = 0;
    if (!PyArg_ParseTuple(args, "Kk", &address, &size))
        Py_RETURN_NONE;

    if (debugPyCdbextModule)
        DebugPrint() << "Read raw memory: " << size << "bytes from " << std::hex << std::showbase << address;

    char *buffer = new char[size];

    CIDebugDataSpaces *data = ExtensionCommandContext::instance()->dataSpaces();
    ULONG bytesWritten = 0;
    HRESULT hr = data->ReadVirtual(address, buffer, size, &bytesWritten);
    if (FAILED(hr))
        bytesWritten = 0;
    PyObject *ret = Py_BuildValue("y#", buffer, bytesWritten);
    delete[] buffer;
    return ret;
}

static PyObject *cdbext_createValue(PyObject *, PyObject *args)
{
    ULONG64 address = 0;
    Type *type = 0;
    if (!PyArg_ParseTuple(args, "KO", &address, &type))
        Py_RETURN_NONE;

    if (debugPyCdbextModule) {
        DebugPrint() << "Create Value address: 0x" << std::hex << address
                     << " type name: " << getTypeName(type);
    }

    IDebugSymbolGroup2 *symbolGroup = currentSymbolGroup.get();
    if (symbolGroup == nullptr)
        Py_RETURN_NONE;

    ULONG numberOfSymbols = 0;
    symbolGroup->GetNumberSymbols(&numberOfSymbols);
    ULONG index = 0;
    for (;index < numberOfSymbols; ++index) {
        ULONG64 offset;
        symbolGroup->GetSymbolOffset(index, &offset);
        if (offset == address) {
            DEBUG_SYMBOL_PARAMETERS params;
            if (SUCCEEDED(symbolGroup->GetSymbolParameters(index, 1, &params))) {
                if (params.TypeId == type->m_typeId && params.Module == type->m_module)
                    break;
            }
        }
    }

    if (index >= numberOfSymbols) {
        ULONG index = DEBUG_ANY_ID;
        const std::string name = SymbolGroupValue::pointedToSymbolName(address, getTypeName(type, true));
        if (debugPyCdbextModule)
            DebugPrint() << "Create Value expression: " << name;

        if (FAILED(symbolGroup->AddSymbol(name.c_str(), &index)))
            Py_RETURN_NONE;
    }

    return createValue(index, symbolGroup);
}

static PyMethodDef cdbextMethods[] = {
    {"parseAndEvaluate",    cdbext_parseAndEvaluate,    METH_VARARGS,
     "Returns value of expression or None if the expression can not be resolved"},
    {"lookupType",          cdbext_lookupType,          METH_VARARGS,
     "Returns type object or None if the type can not be resolved"},
    {"listOfLocals",        cdbext_listOfLocals,        METH_VARARGS,
     "Returns list of values that are currently in scope"},
    {"pointerSize",         cdbext_pointerSize,         METH_NOARGS,
     "Returns the size of a pointer"},
    {"readRawMemory",       cdbext_readRawMemory,       METH_VARARGS,
     "Read a block of data from the virtual address space"},
    {"createValue",         cdbext_createValue,         METH_VARARGS,
     "Creates a value with the given type at the given address"},
    {NULL,                  NULL,               0,
     NULL}        /* Sentinel */
};

static struct PyModuleDef cdbextModule = {
   PyModuleDef_HEAD_INIT,
   "cdbext",                                /* name of module */
   "bridge to the creator cdb extension",   /* module documentation */
   -1,       /* size of per-interpreter state of the module,
                or -1 if the module keeps state in global variables. */
   cdbextMethods
};

PyMODINIT_FUNC
PyInit_cdbext(void)
{
    if (PyType_Ready(field_pytype()) < 0)
        Py_RETURN_NONE;

    if (PyType_Ready(type_pytype()) < 0)
        Py_RETURN_NONE;

    if (PyType_Ready(value_pytype()) < 0)
        Py_RETURN_NONE;

    stdoutRedirect_pytype()->tp_new = PyType_GenericNew;
    if (PyType_Ready(stdoutRedirect_pytype()) < 0)
        Py_RETURN_NONE;

    PyObject *module = PyModule_Create(&cdbextModule);
    if (module == NULL)
        Py_RETURN_NONE;

    Py_INCREF(field_pytype());
    Py_INCREF(stdoutRedirect_pytype());
    Py_INCREF(type_pytype());
    Py_INCREF(value_pytype());

    PyModule_AddObject(module, "Field",
                       reinterpret_cast<PyObject *>(field_pytype()));
    PyModule_AddObject(module, "StdoutRedirect",
                       reinterpret_cast<PyObject *>(stdoutRedirect_pytype()));
    PyModule_AddObject(module, "Type",
                       reinterpret_cast<PyObject *>(type_pytype()));
    PyModule_AddObject(module, "Value",
                       reinterpret_cast<PyObject *>(value_pytype()));

    return module;
}

void initCdbextPythonModule()
{
    PyImport_AppendInittab("cdbext", PyInit_cdbext);
}

PyObject *pyBool(bool b)
{
    if (b)
        Py_RETURN_TRUE;
    else
        Py_RETURN_FALSE;
}

int pointerSize()
{
    return ExtensionCommandContext::instance()->control()->IsPointer64Bit() == S_OK ? 8 : 4;
}

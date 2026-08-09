//
// Copyright (c) 2009, Jim Hourihan
// All rights reserved.
//
// SPDX-License-Identifier: Apache-2.0
//
#ifndef __MuQt__SignalSpy__h__
#define __MuQt__SignalSpy__h__
#include <iostream>
#include <Mu/FunctionObject.h>
#include <Mu/Thread.h>
#include <QtCore/QObject>
#include <QtCore/QMetaMethod>
#include <QtCore/QList>
#include <QtCore/QByteArray>
#include <QtTest/QSignalSpy>

namespace Mu
{

    // Qt 6.5+ (and fully in 6.11): QSignalSpy is no longer a QObject. SignalSpy
    // therefore inherits QObject itself and composes connection metadata that used
    // to come from QSignalSpy's QObject base + moc.
    class SignalSpy : public QObject
    {
        Q_OBJECT

    public:
        enum Types
        {
            UnknownArg,
            IntArg,
            StringArg,
            BoolArg,
            PointArg,
            ObjectArg,
            ActionArg,
            ColorArg,
            TreeItemArg,
            ListItemArg,
            TableItemArg,
            StandardItemArg,
            ModelIndexArg,
            ItemSelectionArg,
            UrlArg,
            VariantArg
        };

        SignalSpy(QObject*, const char* signal, const Function* F, Process* p);

        virtual ~SignalSpy();

        int original_qt_metacall(QMetaObject::Call, int, void**);

        // QMetaType ids for each signal parameter (replaces QSignalSpy::args).
        const QList<int>& argMetaTypes() const { return _argMetaTypes; }

    private:
        bool connectToSignal(QObject* object, const char* signal);

        const Function* _F;
        Process* _process;
        const CallEnvironment* _env;
        std::vector<Types> _argTypes;
        QList<int> _argMetaTypes;
        QByteArray _signalName;
        QMetaObject::Connection _connection;
    };

} // namespace Mu

#endif // __MuQt__SignalSpy__h__

/*
 * Copyright (C) 2006 Apple Computer, Inc.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "SQLiteTransaction.h"

#include "SQLiteDatabase.h"

namespace WebCore {

SQLiteTransaction::SQLiteTransaction(SQLiteDatabase& db, bool readOnly)
    : m_db(db)
    , m_inProgress(false)
    , m_readOnly(readOnly)
{
}

SQLiteTransaction::~SQLiteTransaction()
{
    if (m_inProgress)
        rollback();
}

void SQLiteTransaction::begin()
{
    if (!m_inProgress) {
        ASSERT(!m_db.m_transactionInProgress);
        // For read-only transactions, call BEGIN DEFERRED explicitly.
        // Otherwise, a port might use a SQLite library that was build
        // to interpret BEGIN as BEGIN IMMEDIATE, which would lead to
        // a deadlock whenever two read transactions on the same DB
        // are scheduled concurrently.
        // For write transactions, call BEGIN IMMEDIATE to acquire
        // a RESERVED lock on the DB file before the transaction
        // callback is executed. Otherwise, another write transaction
        // (on another connection) could make changes to the same DB file
        // before this transaction gets to execute any statements.
        // If that happens, this transaction will fail.
        // http://www.sqlite.org/lang_transaction.html
        // http://www.sqlite.org/lockingv3.html#locking
        if (m_readOnly)
            m_inProgress = m_db.executeCommand("BEGIN DEFERRED;");
        else
            m_inProgress = m_db.executeCommand("BEGIN IMMEDIATE;");
        m_db.m_transactionInProgress = m_inProgress;
    }
}

void SQLiteTransaction::commit()
{
    if (m_inProgress) {
        ASSERT(m_db.m_transactionInProgress);
        m_db.executeCommand("COMMIT;");
        m_inProgress = false;
        m_db.m_transactionInProgress = false;
    }
}

void SQLiteTransaction::rollback()
{
    if (m_inProgress) {
        ASSERT(m_db.m_transactionInProgress);
        m_db.executeCommand("ROLLBACK;");
        m_inProgress = false;
        m_db.m_transactionInProgress = false;
    }
}

void SQLiteTransaction::stop()
{
    m_inProgress = false;
    m_db.m_transactionInProgress = false;
}

} // namespace WebCore

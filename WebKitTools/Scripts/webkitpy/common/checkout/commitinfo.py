# Copyright (c) 2010 Google Inc. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#     * Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above
# copyright notice, this list of conditions and the following disclaimer
# in the documentation and/or other materials provided with the
# distribution.
#     * Neither the name of Google Inc. nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
# WebKit's python module for holding information on a commit

import StringIO

from webkitpy.common.config.committers import CommitterList
from webkitpy.common.net.bugzilla import parse_bug_id


class CommitInfo(object):
    def __init__(self, revision, committer_email, changelog_data, committer_list=CommitterList()):
        self._revision = revision
        self._committer_email = committer_email
        self._bug_id = changelog_data["bug_id"]
        self._author_name = changelog_data["author_name"]
        self._author_email = changelog_data["author_email"]
        self._author = changelog_data["author"]
        self._reviewer_text = changelog_data["reviewer_text"]
        self._reviewer = changelog_data["reviewer"]

        # Derived values:
        self._committer = committer_list.committer_by_email(committer_email)

    def revision(self):
        return self._revision

    def committer(self):
        return self._committer # Should never be None

    def committer_email(self):
        return self._committer_email

    def bug_id(self):
        return self._bug_id # May be None

    def author(self):
        return self._author # May be None

    def author_name(self):
        return self._author_name

    def author_email(self):
        return self._author_email

    def reviewer(self):
        return self._reviewer # May be None

    def reviewer_text(self):
        return self._reviewer_text # May be None

/*
 * An implementation of the RAFT protocol.
 *
 * Copyright (C) 2017 Nextworks
 * Author: Vincenzo Maffione <v.maffione@gmail.com>
 *
 * This file is part of rlite.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include "rlite/raft.hpp"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

using namespace std;

int
RaftSM::init(const list<ReplicaId> peers, RaftSMOutput *out)
{
    /* If logfile does not exists it means that this is the first time
     * this replica boots. */
    bool first_boot              = !ifstream(logfilename).good();
    std::ios_base::openmode mode = ios::in | ios::out | ios::binary;
    int ret;

    if (check_output_arg(out)) {
        return -1;
    }

    /* Check if log_entry_size was valid. */
    if (log_entry_size <= sizeof(Term)) {
        IOS_ERR() << "Log entry size " << log_entry_size << " is too short"
                  << endl;
        return -1;
    }

    if (first_boot) {
        mode |= ios::trunc;
    } else {
        mode |= ios::ate;
    }

    logfile.open(logfilename, mode);
    if (logfile.fail()) {
        IOS_ERR() << "Failed to open logfile '" << logfilename
                  << "': " << strerror(errno) << endl;
        return -1;
    }
    if (first_boot) {
        char null[kLogVotedForSize];

        /* Initialize the log header. Write an 4 byte magic
         * number, a 4 bytes current_term and a NULL voted_for. */
        if ((ret = log_u32_write(kLogMagicOfs, kLogMagicNumber))) {
            return ret;
        }
        if ((ret = log_u32_write(kLogCurrentTermOfs, 0))) {
            return ret;
        }
        memset(null, 0, sizeof(null));
        if ((ret = log_buf_write(kLogVotedForOfs, null, kLogVotedForSize))) {
            return ret;
        }
        last_log_index = 0;
        IOS_INF() << "Raft log initialized on first boot" << endl;

    } else {
        char id_buf[kLogVotedForSize];
        long log_size;

        /* Get pointer is at the end because of ios::ate. We can then compute
         * the last log entry. */
        log_size = static_cast<long>(logfile.tellg()) - kLogEntriesOfs;
        if (log_size < 0 || log_size % log_entry_size != 0) {
            IOS_ERR() << "Log size " << log_size << " is invalid" << endl;
            return -1;
        }
        last_log_index = log_size / log_entry_size;

        /* Check the magic number and load current term and current
         * voted candidate. */
        if ((ret = magic_check())) {
            IOS_ERR() << "Log content is corrupted or invalid" << endl;
            return ret;
        }
        if ((ret = log_u32_read(kLogCurrentTermOfs, &current_term))) {
            return ret;
        }
        if ((ret = log_buf_read(kLogVotedForOfs, id_buf, kLogVotedForSize))) {
            return ret;
        }
        /* Check that the 'voted_for' field on disk is null terminated. */
        auto buf_is_null_terminated = [](const char *buf, size_t len) -> bool {
            for (size_t i = 0; i < len; i++) {
                if (buf[i] == '\0') {
                    return true;
                }
            }
            return false;
        };
        if (!buf_is_null_terminated(id_buf, kLogVotedForSize)) {
            IOS_ERR() << "Log contains an invalid voted_for field" << endl;
            return -1;
        }
        voted_for               = string(id_buf);
        auto voted_for_is_valid = [this, peers]() -> bool {
            if (voted_for.empty()) {
                return true;
            }
            if (voted_for == local_id) {
                return true;
            }
            for (const auto &peer : peers) {
                if (voted_for == peer) {
                    return true;
                }
            }
            return false;
        };
        if (!voted_for_is_valid()) {
            IOS_ERR() << "Log contains a voted_for identifier that does not "
                         "match any replica"
                      << endl;
            return -1;
        }
        IOS_INF() << "Raft log recovered" << endl;
    }

    for (const auto &rid : peers) {
        match_index[rid] = 0;
        next_index[rid]  = last_log_index + 1;
    }

    /* Initialization is complete, we can set the election timer and return to
     * the caller. */
    out->timer_commands.push_back(RaftTimerCmd(RaftTimerType::Election,
                                               RaftTimerAction::Set,
                                               rand_int_in_range(10, 50)));

    return 0;
}

void
RaftSM::shutdown()
{
    if (remove(logfilename.c_str())) {
        IOS_ERR() << "Failed to remove log file '" << logfilename
                  << "': " << strerror(errno) << endl;
    }
}

RaftSM::~RaftSM()
{
    if (logfile.is_open()) {
        logfile.close();
    }
}

int
RaftSM::log_u32_write(unsigned long pos, uint32_t val)
{
    logfile.seekp(pos);
    if (logfile.fail()) {
        IOS_ERR() << "Failed to seek log at position " << pos << endl;
        return -1;
    }
    logfile.write(reinterpret_cast<const char *>(&val), sizeof(val));
    if (logfile.fail()) {
        IOS_ERR() << "Failed to write u32 at position " << pos << endl;
        return -1;
    }
    return 0;
}

int
RaftSM::log_u32_read(unsigned long pos, uint32_t *val)
{
    logfile.seekg(pos);
    if (logfile.fail() || logfile.eof()) {
        IOS_ERR() << "Failed to seek log at position " << pos << endl;
        return -1;
    }
    logfile.read(reinterpret_cast<char *>(val), sizeof(*val));
    if (logfile.fail() || logfile.eof()) {
        IOS_ERR() << "Failed to read u32 at position " << pos << endl;
        return -1;
    }
    return 0;
}

int
RaftSM::magic_check()
{
    uint32_t magic = 0;

    if (log_u32_read(kLogMagicOfs, &magic)) {
        return -1;
    }
    return (magic != kLogMagicNumber) ? -1 : 0;
}

int
RaftSM::log_buf_write(unsigned long pos, const char *buf, size_t len)
{
    logfile.seekp(pos);
    if (logfile.fail()) {
        IOS_ERR() << "Failed to seek log at position " << pos << endl;
        return -1;
    }
    logfile.write(buf, len);
    if (logfile.fail()) {
        IOS_ERR() << "Failed to write " << len << " bytes at position " << pos
                  << endl;
        return -1;
    }
    return 0;
}

int
RaftSM::log_buf_read(unsigned long pos, char *buf, size_t len)
{
    logfile.seekg(pos);
    if (logfile.fail() || logfile.eof()) {
        IOS_ERR() << "Failed to seek log at position " << pos << endl;
        return -1;
    }
    logfile.read(buf, len);
    if (logfile.fail() || logfile.eof()) {
        IOS_ERR() << "Failed to read " << len << " bytes at position " << pos
                  << endl;
        return -1;
    }
    return 0;
}

int
RaftSM::rand_int_in_range(int left, int right)
{
    assert(right > left);
    return left + (rand() % (right - left));
}

std::string
RaftSM::state_repr(RaftState st) const
{
    switch (st) {
    case RaftState::Follower:
        return "Follower";
    case RaftState::Candidate:
        return "Candidate";
    case RaftState::Leader:
        return "Leader";
    }

    assert(false);

    return "Unknown";
}

void
RaftSM::switch_state(RaftState next)
{
    if (state == next) {
        return; /* nothing to do */
    }
    IOS_INF() << "switching " << state_repr(state) << " --> "
              << state_repr(next) << endl;
    state = next;
}

int
RaftSM::check_output_arg(RaftSMOutput *out)
{
    if (out == nullptr || !out->output_messages.empty() ||
        !out->timer_commands.empty()) {
        IOS_ERR() << "Invalid output parameter" << endl;
        return -1;
    }

    return 0;
}

/* Updates 'voted_for' persistent data. May be called with an empty string
 * to reset voting state. */
int
RaftSM::vote_for_candidate(ReplicaId candidate)
{
    if (voted_for != candidate) {
        char buf_id[kLogVotedForSize];
        int ret;

        voted_for = candidate;
        snprintf(buf_id, sizeof(buf_id), "%s", voted_for.c_str());
        if ((ret = log_buf_write(kLogVotedForOfs, buf_id, sizeof(buf_id)))) {
            return ret;
        }
    }

    return 0;
}

int
RaftSM::back_to_follower(RaftSMOutput *out)
{
    int ret;

    switch_state(RaftState::Follower);
    votes_collected = 0;
    if ((ret = vote_for_candidate(string()))) {
        return ret;
    }
    out->timer_commands.push_back(RaftTimerCmd(RaftTimerType::Election,
                                               RaftTimerAction::Set,
                                               rand_int_in_range(10, 50)));
    return 0;
}

/* Called on any input message to check if our term is outdated. */
int
RaftSM::catch_up_term(Term term, RaftSMOutput *out)
{
    int ret;

    if (term <= current_term) {
        return 0; /* nothing to do */
    }

    /* Our term is outdated. Updated it and become a follower. */
    IOS_INF() << "Update current term " << current_term << " --> " << term
              << endl;
    current_term = term;
    if ((ret = log_u32_write(kLogCurrentTermOfs, current_term))) {
        return ret;
    }
    if ((ret = back_to_follower(out))) {
        return ret;
    }

    return 1;
}

int
RaftSM::request_vote_input(const RaftRequestVote &msg, RaftSMOutput *out)
{
    RaftRequestVoteResp *resp = nullptr;
    int ret;

    if (check_output_arg(out)) {
        return -1;
    }

    IOS_INF() << "Received VoteRequest(term=" << msg.term
              << ", cand=" << msg.candidate_id
              << ", last_log_term=" << msg.last_log_term
              << ", last_log_index=" << msg.last_log_index << ")" << endl;

    if ((ret = catch_up_term(msg.term, out))) {
        if (ret < 0) {
            return ret; /* error */
        }
        /* Current term has been updated, go ahead. */
    }

    resp       = new RaftRequestVoteResp();
    resp->term = current_term;

    if (msg.term < current_term) {
        /* Received message belonging to an outdated term. Reply with a
         * nack and the updated term. */

        resp->vote_granted = false;
    } else {
        /* We grant our vote if we haven't voted for anyone in this term (or
         * we already voted for the candidate) and the candidate's log is at
         * least as up-to-date as ours. */
        resp->vote_granted =
            (voted_for.empty() || voted_for == msg.candidate_id) &&
            (msg.last_log_term > last_log_term ||
             (msg.last_log_term == last_log_term &&
              msg.last_log_index >= last_log_index));
    }

    if (resp->vote_granted && voted_for.empty()) {
        /* Register the vote on peristent memory. */
        if ((ret = vote_for_candidate(msg.candidate_id))) {
            return ret;
        }
    }
    IOS_INF() << "Vote for " << msg.candidate_id
              << (resp->vote_granted ? "" : " not") << " granted" << endl;

    out->output_messages.push_back(make_pair(msg.candidate_id, resp));

    return 0;
}

int
RaftSM::request_vote_resp_input(const RaftRequestVote &msg, RaftSMOutput *out)
{
    int ret;

    if (check_output_arg(out)) {
        return -1;
    }

    if ((ret = catch_up_term(msg.term, out))) {
        if (ret < 0) {
            return ret; /* error */
        }

        return 0;
    }

    return 0;
}

int
RaftSM::append_entries_input(const RaftAppendEntries &msg, RaftSMOutput *out)
{
    if (check_output_arg(out)) {
        return -1;
    }

    return 0;
}

int
RaftSM::append_entries_resp_input(const RaftAppendEntries &msg,
                                  RaftSMOutput *out)
{
    if (check_output_arg(out)) {
        return -1;
    }

    return 0;
}

int
RaftSM::timer_expired(RaftTimerType type, RaftSMOutput *out)
{
    int ret;

    if (check_output_arg(out)) {
        return -1;
    }

    if (type == RaftTimerType::Election) {
        /* The election timer fired. */
        IOS_INF() << "Election timer expired" << endl;
        if (state == RaftState::Leader) {
            /* Nothing to do. */
            return 0;
        }
        /* Switch to candidate and increment current term. */
        switch_state(RaftState::Candidate);
        if ((ret = log_u32_write(kLogCurrentTermOfs, ++current_term))) {
            return ret;
        }
        /* Vote for myself. */
        if ((ret = vote_for_candidate(local_id))) {
            return ret;
        }
        votes_collected = 1;
        /* Reset the election timer in case we lose the election. */
        out->timer_commands.push_back(RaftTimerCmd(RaftTimerType::Election,
                                                   RaftTimerAction::Set,
                                                   rand_int_in_range(10, 50)));
        /* Prepare RequestVote messages for the other servers. */
        for (const auto &kv : next_index) {
            auto *msg           = new RaftRequestVote();
            msg->candidate_id   = local_id;
            msg->last_log_index = last_log_index;
            msg->last_log_term  = last_log_term;
            out->output_messages.push_back(make_pair(kv.first, msg));
        }
    }

    return 0;
}
/*
 * Copyright (c) 2012 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2006 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __CPU_O3_DEP_GRAPH_HH__
#define __CPU_O3_DEP_GRAPH_HH__

#include "cpu/o3/comm.hh"
#include <unordered_map>

namespace gem5
{

namespace o3
{

// Custom hash function for ListIt
struct ListItHash {
    std::size_t operator()(const std::list<DynInstPtr>::iterator &it) const {
        return std::hash<DynInstPtr *>()(&(*it));
    }
};

/** Node in a linked list. */
template <class DynInstPtr>
class DependencyEntry
{
  public:
    DependencyEntry()
        : inst(NULL), src_idx(-1), next(NULL)
    { }

    DynInstPtr inst;
    //Might want to include data about what arch. register the
    //dependence is waiting on.
    int src_idx;
    DependencyEntry<DynInstPtr> *next;
};

/** @deprecated Array of linked list that maintains the dependencies between
 * producing instructions and consuming instructions.  Each linked
 * list represents a single physical register, having the future
 * producer of the register's value, and all consumers waiting on that
 * value on the list.  The head node of each linked list represents
 * the producing instruction of that register.  Instructions are put
 * on the list upon reaching the IQ, and are removed from the list
 * either when the producer completes, or the instruction is squashed.
 * @note Dependency graph using unordered_map with InstSeqNum as the key.
*/
template <class DynInstPtr>
class DependencyGraph
{
  public:
    typedef DependencyEntry<DynInstPtr> DepEntry;
    typedef uint64_t InstSeqNum;

    DependencyGraph()
        : memAllocCounter(0), nodesTraversed(0), nodesRemoved(0)
    { }

    ~DependencyGraph();

    /** Clears all of the linked lists. */
    void reset();

    void insert(InstSeqNum seq_num, const DynInstPtr &new_inst, RegIndex idx);

    void setInst(InstSeqNum seq_num, const DynInstPtr &new_inst)
    { dependGraph[seq_num].inst = new_inst; }

    void clearInst(InstSeqNum seq_num)
    { dependGraph[seq_num].inst = NULL; }

    void remove(InstSeqNum seq_num, const DynInstPtr &inst_to_remove);

    DynInstPtr pop(InstSeqNum seq_num, int *idx);

    bool empty() const;

    bool empty(InstSeqNum seq_num) const { 
        auto iter = dependGraph.find(seq_num);
        return iter == dependGraph.end() || !iter->second.next; 
    }

    void dump();

  private:
    std::unordered_map<InstSeqNum, DepEntry> dependGraph;

    /** @deprecated 
     * Number of linked lists; identical to the number of registers.
     */
    // int numEntries;

    // Debug variable, remove when done testing.
    unsigned memAllocCounter;

  public:
    // Debug variable, remove when done testing.
    uint64_t nodesTraversed;
    // Debug variable, remove when done testing.
    uint64_t nodesRemoved;
};

template <class DynInstPtr>
DependencyGraph<DynInstPtr>::~DependencyGraph()
{
}

template <class DynInstPtr>
void
DependencyGraph<DynInstPtr>::reset()
{
    for (auto &entry : dependGraph) {
        DepEntry *curr = entry.second.next;
        while (curr) {
            memAllocCounter--;
            DepEntry *prev = curr;
            curr = curr->next;
            prev->inst = NULL;
            delete prev;
        }
        entry.second.inst = NULL;
        entry.second.next = NULL;
    }
    dependGraph.clear();
}

template <class DynInstPtr>
void
DependencyGraph<DynInstPtr>::insert(InstSeqNum seq_num, const DynInstPtr &new_inst, RegIndex idx)
{
    //Add this new, dependent instruction at the head of the dependency
    //chain.

    // First create the entry that will be added to the head of the
    // dependency chain.
    DepEntry *new_entry = new DepEntry;
    new_entry->next = dependGraph[seq_num].next;
    new_entry->inst = new_inst;
    new_entry->src_idx = idx;
    dependGraph[seq_num].next = new_entry;
    ++memAllocCounter;
}

template <class DynInstPtr>
void
DependencyGraph<DynInstPtr>::remove(InstSeqNum seq_num, const DynInstPtr &inst_to_remove)
{
    DepEntry *prev = &dependGraph[seq_num];
    DepEntry *curr = dependGraph[seq_num].next;

    // Make sure curr isn't NULL.  Because this instruction is being
    // removed from a dependency list, it must have been placed there at
    // an earlier time.  The dependency chain should not be empty,
    // unless the instruction dependent upon it is already ready.
    if (curr == NULL) {
        return;
    }

    nodesRemoved++;

    // Find the instruction to remove within the dependency linked list.
    while (curr->inst != inst_to_remove) {
        prev = curr;
        curr = curr->next;
        nodesTraversed++;

        assert(curr != NULL);
    }

    // Now remove this instruction from the list.
    prev->next = curr->next;

    --memAllocCounter;

    // Could push this off to the destructor of DependencyEntry
    curr->inst = NULL;

    delete curr;
}

template <class DynInstPtr>
DynInstPtr
DependencyGraph<DynInstPtr>::pop(InstSeqNum seq_num, int *idx)
{
    DepEntry *node = dependGraph[seq_num].next;
    DynInstPtr inst = NULL;
    if (node) {
        inst = node->inst;
        *idx = node->src_idx;
        dependGraph[seq_num].next = node->next;
        node->inst = NULL;
        memAllocCounter--;
        delete node;
    }
    return inst;
}

template <class DynInstPtr>
bool
DependencyGraph<DynInstPtr>::empty() const
{
    return dependGraph.empty();
}

template <class DynInstPtr>
void
DependencyGraph<DynInstPtr>::dump()
{
    for (const auto &entry : dependGraph) {
        const DepEntry *curr = &entry.second;
        if (curr->inst) {
            cprintf("dependGraph[%llu]: producer: %s [sn:%lli] consumer: ",
                    entry.first, curr->inst->pcState(), curr->inst->seqNum);
        } else {
            cprintf("dependGraph[%llu]: No producer. consumer: ", entry.first);
        }

        while (curr->next != NULL) {
            curr = curr->next;
            cprintf("%s [sn:%lli] ",
                    curr->inst->pcState(), curr->inst->seqNum);
        }

        cprintf("\n");
    }
    cprintf("memAllocCounter: %i\n", memAllocCounter);
}

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_DEP_GRAPH_HH__

# PES-VCS Analysis Questions - Answers

## Phase 5: Branching and Checkout

### Q5.1: Implementing `pes checkout <branch>`

**How would you implement `pes checkout <branch>` — what files need to change in `.pes/`, and what must happen to the working directory? What makes this operation complex?**

**Answer:**

To implement `pes checkout <branch>`, the following changes are needed:

**Files to change in `.pes/`:**
1. **`.pes/HEAD`** - Update to point to the new branch reference
   - Change from `ref: refs/heads/main` to `ref: refs/heads/<branch>`
   - This is an atomic write operation (temp file + rename)

**Working directory updates:**
1. Read the target branch's commit hash from `.pes/refs/heads/<branch>`
2. Load that commit object and get its root tree hash
3. Recursively traverse the tree to reconstruct all files:
   - For each blob entry: write the file contents to the working directory
   - For each tree entry: create the directory and recurse
4. Remove files that exist in the current tree but not in the target tree
5. Update `.pes/index` to match the new tree state

**What makes this complex:**
- **File deletion safety**: Must detect uncommitted changes before deleting files
- **Directory handling**: Need to create/remove directories in correct order (depth-first)
- **Concurrent modifications**: User might edit files during checkout
- **Permissions**: Must restore executable bits correctly
- **Symlinks**: Need special handling for symbolic links
- **Partial failures**: If checkout fails halfway, need to roll back or continue
- **Performance**: Large repositories require efficient tree traversal
- **Platform differences**: File permissions and paths differ on Windows vs Unix

---

### Q5.2: Detecting "dirty working directory" conflicts

**When switching branches, the working directory must be updated to match the target branch's tree. If the user has uncommitted changes to a tracked file, and that file differs between branches, checkout must refuse. Describe how you would detect this "dirty working directory" conflict using only the index and the object store.**

**Answer:**

**Detection Algorithm:**

1. **Load current HEAD's tree** (call it `tree_current`)
2. **Load target branch's tree** (call it `tree_target`)
3. **Load the index**
4. **For each file in the working directory:**
   
   a. **Check if file is tracked** (exists in index)
   
   b. **Compute current file's hash** by reading and hashing its contents
   
   c. **Get the indexed hash** from `.pes/index`
   
   d. **Compare hashes:**
      - If `current_hash ≠ indexed_hash`: File has **uncommitted changes**
   
   e. **Check if this file differs between branches:**
      - Look up the file path in `tree_current` → get `hash_current`
      - Look up the file path in `tree_target` → get `hash_target`
      - If `hash_current ≠ hash_target` AND file has uncommitted changes:
        → **CONFLICT DETECTED** - refuse checkout
   
   f. **Special case - untracked files:**
      - If file exists in `tree_target` but not in index or `tree_current`
      - Would be overwritten by checkout → refuse

**Implementation details:**
```
for each path in working_directory:
    if path in index:
        working_hash = hash(read_file(path))
        index_hash = index[path].hash
        
        if working_hash != index_hash:  # Uncommitted changes
            current_tree_hash = lookup_in_tree(tree_current, path)
            target_tree_hash = lookup_in_tree(tree_target, path)
            
            if current_tree_hash != target_tree_hash:
                error("Your local changes to '%s' would be overwritten by checkout", path)
                return CONFLICT
```

**Why this works without re-reading the entire working tree:**
- The index already knows the last-staged hash for each file
- Quick metadata check (mtime, size) can skip hashing unchanged files
- Only files that differ between branches AND have uncommitted changes cause conflicts

---

### Q5.3: Detached HEAD and commit recovery

**"Detached HEAD" means HEAD contains a commit hash directly instead of a branch reference. What happens if you make commits in this state? How could a user recover those commits?**

**Answer:**

**What happens when committing in detached HEAD state:**

1. **Commits are created normally** with proper parent pointers
2. **HEAD is updated** to point directly to the new commit hash
3. **No branch reference is updated** - commits are "orphaned"
4. **When checking out a branch**, HEAD moves to that branch, and the detached commits become unreachable

**Example:**
```
Initial state:
  main → C3 → C2 → C1
  HEAD → main

Detach at C2:
  main → C3 → C2 → C1
  HEAD → C2 (detached)

Make commits C4, C5:
  main → C3 → C2 → C1
             ↓
            C4 → C5
             ↑
           HEAD (detached)

Checkout main:
  main → C3 → C2 → C1
  HEAD → main
  
  C4 → C5 (unreachable, will be garbage collected)
```

**Recovery methods:**

**1. If you remember the commit hash:**
```bash
./pes checkout -b recovery-branch <commit-hash>
```
Creates a new branch pointing to the orphaned commit.

**2. Using reflog (if implemented):**
```bash
./pes reflog  # Shows recent HEAD movements
./pes checkout -b recovery-branch HEAD@{2}
```

**3. Manual object store search:**
```bash
# Find recent commit objects
find .pes/objects -name "*" -type f -mtime -1

# For each recent object, try to read it as a commit
# Check if the message matches what you remember
# Create branch pointing to it
```

**4. If commits are still in memory/recent:**
- Before garbage collection runs, commits are still in `.pes/objects/`
- Can scan for commits created in the last N hours
- Parse commit messages to identify the right one

**Prevention:**
- Warn users when entering detached HEAD state
- Automatically create temporary branch `detached-<hash>` on first commit
- Keep a reflog of all HEAD movements

---

## Phase 6: Garbage Collection and Space Reclamation

### Q6.1: Finding and deleting unreachable objects

**Over time, the object store accumulates unreachable objects — blobs, trees, or commits that no branch points to (directly or transitively). Describe an algorithm to find and delete these objects. What data structure would you use to track "reachable" hashes efficiently? For a repository with 100,000 commits and 50 branches, estimate how many objects you'd need to visit.**

**Answer:**

**Algorithm: Mark and Sweep Garbage Collection**

**Phase 1: Mark (find all reachable objects)**

```
1. Initialize: Create empty hash set `reachable`

2. For each reference (branch, tag, HEAD):
   - Read the commit hash
   - Call mark_commit(hash)

3. mark_commit(commit_hash):
   - If commit_hash already in `reachable`: return (already visited)
   - Add commit_hash to `reachable`
   - Read commit object
   - Extract tree_hash → call mark_tree(tree_hash)
   - If has parent: call mark_commit(parent_hash)  # Recursive

4. mark_tree(tree_hash):
   - If tree_hash already in `reachable`: return
   - Add tree_hash to `reachable`
   - Read tree object
   - For each entry:
     - If entry is blob: add blob_hash to `reachable`
     - If entry is tree: call mark_tree(entry.hash)  # Recursive

5. Result: `reachable` set contains all accessible objects
```

**Phase 2: Sweep (delete unreachable objects)**

```
1. List all objects in .pes/objects/
2. For each object hash:
   - If hash NOT in `reachable`:
     - Delete the object file
     - Log for potential undo
```

**Data structure choice: Hash Set**
- **Why**: O(1) lookup to check if an object is reachable
- **Alternatives**:
  - Bloom filter: Memory-efficient, but false positives
  - Sorted array: O(log n) lookup, but simpler implementation
  - Bitmap: If hashes are sequential (not practical for SHA-256)

**Space complexity:**
- Hash set with 100,000 commits: ~3.2 MB (32 bytes per hash)
- Plus trees and blobs: Assume 10× more objects = 32 MB
- Totally acceptable for modern systems

**Object visitation estimate for 100,000 commits, 50 branches:**

Assumptions:
- Average 5 files modified per commit
- Average directory depth: 3 levels
- 50 branches, average divergence: 10 commits

**Calculation:**
1. **Commits**: Need to visit all 100,000 commits (follow parent pointers)
2. **Trees**: Each commit has 1 root tree + ~3 subtrees = 4 trees
   - Total: 100,000 × 4 = 400,000 trees
   - But many are shared (unchanged directories)
   - Estimate unique: ~150,000 trees
3. **Blobs**: 100,000 commits × 5 files = 500,000 file versions
   - But with deduplication (same content): ~200,000 unique blobs

**Total objects to visit: ~450,000**
- Commits: 100,000
- Trees: 150,000
- Blobs: 200,000

**Runtime**: With hash set lookups (O(1)), mark phase is linear in objects visited.
- At 1000 objects/second: ~450 seconds = 7.5 minutes
- With optimizations (parallel tree traversal): <2 minutes

---

### Q6.2: Race conditions with concurrent operations

**Why is it dangerous to run garbage collection concurrently with a commit operation? Describe a race condition where GC could delete an object that a concurrent commit is about to reference. How does Git's real GC avoid this?**

**Answer:**

**Dangerous Race Condition Example:**

**Timeline:**
```
Time  | Commit Process                    | GC Process
------|-----------------------------------|----------------------------------
T0    | Start creating commit C4          |
T1    | Write blob B1                     |
T2    | Hash = abc123                     |
T3    | Write tree T1 referencing B1      |
T4    |                                   | Start GC
T5    |                                   | Scan all branches → no C4 yet
T6    |                                   | Mark phase: B1 not reachable
T7    | Prepare commit object C4          |
T8    |                                   | Sweep phase: DELETE B1 ❌
T9    | Write commit C4 (references T1)   |
T10   | Update HEAD → C4                  |
T11   | **CORRUPTION**: C4 points to      |
      | missing tree T1, which points to  |
      | deleted blob B1                   |
```

**Why this happens:**
1. Objects are written BEFORE the commit becomes reachable
2. GC doesn't see the commit yet (HEAD not updated)
3. GC deletes "unreachable" objects that are actually needed
4. Commit finishes and references deleted objects
5. Repository is now corrupt

**Additional race scenarios:**

**Scenario 2: Tree object deletion**
- Commit writes tree objects bottom-up
- GC runs between tree writes
- Deletes "orphaned" subtrees before root tree is written

**Scenario 3: Two-phase commit problem**
- Commit creates objects (phase 1)
- Updates references (phase 2)
- GC runs between phases → deletes phase 1 objects

---

**How Git Avoids This:**

**1. Grace period (staleness threshold):**
- GC only deletes objects older than 2 weeks (default)
- Recently created objects are never deleted
- Gives time for ongoing operations to complete
```bash
git gc --prune=2.weeks.ago
```

**2. Process-level locking:**
- Create `.git/gc.pid` lock file when GC starts
- Commit operations check for this lock
- Either wait or fail if GC is running
- Lock released when GC completes

**3. Reference logs (reflog):**
- Keep history of all reference updates
- Even "unreachable" commits stay reachable via reflog
- Default expiration: 90 days
- Prevents accidental deletion of recent work

**4. Two-phase garbage collection:**
```
Phase 1: Mark objects as "to be deleted" (rename with .tmp)
Phase 2: Wait for grace period
Phase 3: Actually delete .tmp files
```
- Allows rollback if issues detected

**5. Atomic reference updates:**
- References updated with atomic rename
- Either old or new state is visible, never partial
- GC sees consistent state

**6. Pack files and reachability:**
- Objects in pack files marked differently
- Pack files have their own reachability analysis
- Reduces window for race conditions

**Best practice implementation for PES-VCS:**

```c
int gc_run_safe() {
    // 1. Acquire lock
    int lock_fd = open(".pes/gc.lock", O_CREAT | O_EXCL);
    if (lock_fd < 0) {
        fprintf(stderr, "GC already running or commit in progress\n");
        return -1;
    }
    
    // 2. Mark all reachable objects
    HashSet *reachable = mark_phase();
    
    // 3. Only delete objects older than 2 weeks
    time_t cutoff = time(NULL) - (14 * 24 * 3600);
    
    for each object in .pes/objects:
        if object NOT in reachable:
            if object.mtime < cutoff:  // Grace period
                delete_object(object);
    
    // 4. Release lock
    close(lock_fd);
    unlink(".pes/gc.lock");
    
    return 0;
}

int commit_create(...) {
    // Check if GC is running
    if (access(".pes/gc.lock", F_OK) == 0) {
        fprintf(stderr, "Waiting for GC to complete...\n");
        // Wait or fail
    }
    
    // Proceed with commit
    ...
}
```

---

## Summary

These analysis questions explore advanced VCS concepts:
- **Branching**: File system state management and conflict detection
- **Garbage collection**: Graph reachability and concurrent access safety

Understanding these concepts is crucial for building robust version control systems and appreciating Git's design decisions.

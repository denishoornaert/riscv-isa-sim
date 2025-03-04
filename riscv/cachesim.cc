// See LICENSE for license details.

#include "cachesim.h"
#include "common.h"
#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <algorithm>


cache_sim_t::cache_sim_t(size_t _sets, size_t _ways, size_t _linesz, const char* _name, const std::string eviction_policy)
: sets(_sets), ways(_ways), linesz(_linesz), name(_name), log(false)
{
  init(eviction_policy);
}

cache_sim_t::cache_sim_t(size_t _sets, size_t _ways, size_t _linesz, const char* _name)
: sets(_sets), ways(_ways), linesz(_linesz), name(_name), log(false)
{
  init(std::string("lfsr"));
}

bool cache_sim_t::policy_is_valid(const std::string eviction_policy)
{
  return (!(eviction_policy.compare("lfsr"))) |
         (!(eviction_policy.compare("lru"))) |
         (!(eviction_policy.compare("fifo"))) |
         (!(eviction_policy.compare("lip"))) |
         (!(eviction_policy.compare("bip")));
}

eviction_policy_t* cache_sim_t::create_eviction_policy(const std::string eviction_policy)
{
  eviction_policy_t* policy = NULL;
  if (!(eviction_policy.compare("lfsr")))
    policy = new lfsr_t(sets, ways);
  else if (!(eviction_policy.compare("lru")))
    policy = new lru_t(sets, ways);
  else if (!(eviction_policy.compare("fifo")))
    policy = new fifo_t(sets, ways);
  else if (!(eviction_policy.compare("lip")))
    policy = new lip_t(sets, ways);
  else if (!(eviction_policy.compare("bip")))
    policy = new bip_t(sets, ways);
  return policy;
}

void cache_sim_t::help()
{
  std::cerr << "Cache configurations must be of the form" << std::endl;
  std::cerr << "  sets:ways:blocksize:policy" << std::endl;
  std::cerr << "where sets, ways, and blocksize are positive integers, with" << std::endl;
  std::cerr << "sets and blocksize both powers of two and blocksize at least 8." << std::endl;
  std::cerr << "Finally, policy is a string. Either 'lfsr', 'lru', 'fifo', 'lip', or 'bip'." << std::endl;
  exit(1);
}

cache_sim_t::cache_sim_t(const char* config, const char* name) : name(name)
{
  const char* wp = strchr(config, ':');
  if (!wp++) help();
  const char* bp = strchr(wp, ':');
  if (!bp++) help();
  const char* eviction_policy = strchr(bp, ':');
  if (!eviction_policy++) help();
  if (!policy_is_valid(std::string(eviction_policy))) help();

  sets = atoi(std::string(config, wp).c_str());
  ways = atoi(std::string(wp, bp).c_str());
  linesz = atoi(std::string(bp, eviction_policy).c_str());

  init(eviction_policy);
}

void cache_sim_t::init(const std::string eviction_policy)
{
  if(sets == 0 || (sets & (sets-1)))
    help();
  if(linesz < 8 || (linesz & (linesz-1)))
    help();

  tags.resize(sets);
  for (size_t i = 0; i < tags.size(); i++)
    tags[i].resize(ways);

  perf_counter.set_name(name);

  miss_handler = NULL;
  policy = create_eviction_policy(eviction_policy);
}

cache_sim_t::cache_sim_t(const cache_sim_t& rhs)
 : sets(rhs.sets), ways(rhs.ways), linesz(rhs.linesz),
   tags(rhs.tags), perf_counter(rhs.perf_counter), name(rhs.name), log(false)
{}

cache_sim_t::~cache_sim_t()
{
  delete policy;
}

int cache_sim_t::check_tag(cache_sim_addr_t& addr)
{
  auto begin = tags[addr.idx].begin();
  auto end = tags[addr.idx].end();
  auto it = std::find(begin, end, addr);
  return (likely(it != end))? std::distance(begin, it) : -1;
}

// Returns tag of victimized cacheline AND write new cacheline tag instead of
// the existing one!
cache_sim_addr_t cache_sim_t::victimize(const cache_sim_addr_t& addr)
{
  // Get index of way to evict
  size_t way = policy->next(addr.idx);
  // Store cache-line's tag to be evicted
  cache_sim_addr_t victim = tags[addr.idx][way];
  // Replace evicted cache-line's tag with new one
  tags[addr.idx][way] = addr;
  tags[addr.idx][way].set_valid();
  // Tell the eviction policy which metadata to change
  policy->insert(addr.idx, way);
  return victim;
}

void cache_sim_t::access(const uint64_t raw_addr, const size_t bytes, const bool store)
{
  perf_counter.access(store, bytes);

  cache_sim_addr_t addr = cache_sim_addr_t(raw_addr, this->sets, this->linesz);

  int hit_way_index = check_tag(addr);

  // If cache-hit
  if (likely(hit_way_index >= 0))
  {
    if (store)
      tags[addr.idx][hit_way_index].set_dirty();
    policy->update(addr, hit_way_index);
  }
  // If cache-miss
  else
  {
    perf_counter.miss(store);
    if (log)
    {
      std::cerr << name << " "
                << (store ? "write" : "read") << " miss 0x"
                << std::hex << addr.to_uint64(sets, linesz)
                << std::endl;
    }

    // Victimize AND insert at 'addr'
    cache_sim_addr_t victim = victimize(addr);

    if (victim.is_valid() && victim.is_dirty())
    {
      if (miss_handler)
      {
        uint64_t dirty_addr = victim.to_uint64(sets, linesz);
        miss_handler->access(dirty_addr, linesz, true);
      }
      perf_counter.writeback();
    }

    if (miss_handler)
      miss_handler->access(addr.to_uint64(sets, linesz), linesz, false);

    if (store)
      tags[addr.idx][check_tag(addr)].set_dirty();
  }
}

void cache_sim_t::clean_invalidate(uint64_t addr, size_t bytes, bool clean, bool inval)
{
  cache_sim_addr_t cur_addr = cache_sim_addr_t(addr, this->sets, this->linesz);
  cache_sim_addr_t end_addr = cache_sim_addr_t(addr+bytes, this->sets, this->linesz);
  while (cur_addr < end_addr) {
    int hit_way_index = check_tag(cur_addr);
    if (likely(hit_way_index >= 0))
    {
      if (clean && tags[cur_addr.idx][hit_way_index].is_dirty()) {
        perf_counter.writeback();
        perf_counter.clean();
        tags[cur_addr.idx][hit_way_index].set_clean();
      }

      if (inval)
        tags[cur_addr.idx][hit_way_index].set_invalid();
    }
    cur_addr.next_cacheline(sets);
  }
  if (miss_handler)
    miss_handler->clean_invalidate(addr, bytes, clean, inval);
}

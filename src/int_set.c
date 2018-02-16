/*
 * Copyright 2018 Alex Benishek
 * 
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "int_set.h"
#include <string.h>

/* TODO Add robin hood hashing
   See:
   https://www.sebastiansylvan.com/post/robin-hood-hashing-should-be-your-default-hash-table-implementation/
   http://codecapsule.com/2013/11/11/robin-hood-hashing/
   https://github.com/Lewuathe/robinhood
   https://probablydance.com/2017/02/26/i-wrote-the-fastest-hashtable/
*/

// FNV-1a hash, in the public domain
static uint32_t
hash (uint32_t i)
{
  const uint32_t offset = 2166136261;
  const uint32_t prime = 16777619;
  const uint8_t *end = (uint8_t *) (&i + 1);

  uint32_t hash = offset;
  for (uint8_t *b = (uint8_t *) &i; b != end; b++)
    {
      hash ^= *b;
      hash *= prime;
    }
  return hash;
}

static uint32_t
mask (uint32_t log_alloc)
{
  return ~(((uint32_t) ~0) << log_alloc);
}

static uint32_t
inc_wrap (uint32_t i, uint32_t log)
{
  i++;
  return mask (log) & i;
}

static void
resize (struct int_set *s)
{
  size_t alloc = 1 << s->alloc_log;
  if (s->num * 4 <= alloc * 3)
    return;

  size_t old = alloc;
  alloc = alloc << 1;
  s->alloc_log++;
  s->data = realloc (s->data, alloc);
  s->valid = realloc (s->valid, alloc);
  memset (s->data + old, 0, sizeof (uint32_t));
  memset (s->valid + old, 0, sizeof (bool));
}

void
int_set_init (struct int_set *s)
{
  const uint32_t init_size = 16;
  s->data = calloc (init_size, sizeof (uint32_t));
  s->valid = calloc (init_size, sizeof (bool));
  s->num = 0;
  s->alloc_log = 4;
}

bool
int_set_contains (struct int_set *s, uint32_t i)
{
  resize (s);

  for (uint32_t k = hash(i) & mask (s->alloc_log); s->valid[k]; k = inc_wrap (k, s->alloc_log))
    {
      if (s->data[k] == i)
        return true;
    }
  return false;
}

void
int_set_add (struct int_set *s, uint32_t i)
{
  resize (s);

  uint32_t k;
  for (k = hash (i) & mask (s->alloc_log); s->valid[k]; k = inc_wrap (k, s->alloc_log))
    {
      if (s->data[k] == i)
        return;
    }
  s->data[k] = i;
  s->valid[k] = true;
  s->num++;
}

void
int_set_remove (struct int_set *s, uint32_t i)
{
  resize (s);

  uint32_t k;
  for (k = hash (i) & mask (s->alloc_log); s->valid[k]; k = inc_wrap (k, s->alloc_log))
    {
      if (s->data[k] == i)
        {
          s->valid[k] = false;
          return;
        }
    }
}

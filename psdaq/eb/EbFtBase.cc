#include "EbFtBase.hh"

#include "Endpoint.hh"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace Pds;
using namespace Pds::Fabrics;
using namespace Pds::Eb;


EbFtBase::EbFtBase(unsigned nPeers) :
  _ep(nPeers),
  _mr(nPeers),
  _ra(nPeers),
  _cqPoller(NULL),
  _id(nPeers),
  _mappedId(NULL)
{
}

EbFtBase::~EbFtBase()
{
  if (_mappedId)  delete [] _mappedId;
}

MemoryRegion* EbFtBase::registerMemory(void* buffer, size_t size)
{
  MemoryRegion* mr = NULL;

  for (unsigned i = 0; i < _ep.size(); ++i)
  {
    Endpoint* ep = _ep[i];

    if (!ep)  continue;

    Fabric* fab = ep->fabric();

    mr = fab->register_memory(buffer, size);

    printf("idx: %d, fabric: %p, desc = %p\n", i, fab, mr->desc());
  }

  return mr;
}

void EbFtBase::_mapIds(unsigned nPeers)
{
  unsigned idMax = 0;
  for (unsigned i = 0; i < nPeers; ++i)
    if (_id[i] > idMax) idMax = _id[i];

  _mappedId = new unsigned[idMax + 1];

  for (unsigned i = 0; i < nPeers; ++i)
    _mappedId[_id[i]] = i;
}

int EbFtBase::_syncLclMr(char*          region,
                         size_t         size,
                         Endpoint*      ep,
                         MemoryRegion*  mr,
                         RemoteAddress& ra)
{
  ra = RemoteAddress(mr->rkey(), (uint64_t)region, size);
  memcpy(region, &ra, sizeof(ra));

  if (!ep->send_sync(region, sizeof(ra), mr))
  {
    fprintf(stderr, "Failed sending local memory specs to peer: %s\n",
            ep->error());
    return ep->error_num();
  }

  printf("Local  memory region: %p : %p, size %zd\n",
         (void*)ra.addr, (void*)(ra.addr + ra.extent), ra.extent);

  return 0;
}

int EbFtBase::_syncRmtMr(char*          region,
                         size_t         size,
                         Endpoint*      ep,
                         MemoryRegion*  mr,
                         RemoteAddress& ra)
{
  if (!ep->recv_sync(region, sizeof(ra), mr))
  {
    fprintf(stderr, "Failed receiving remote region specs from peer: %s\n",
            ep->error());
    perror("recv RemoteAddress");
    return ep->error_num();
  }

  memcpy(&ra, region, sizeof(ra));
  if (size > ra.extent)
  {
    fprintf(stderr, "Remote region size (%lu) is less than required (%lu)\n",
            ra.extent, size);
    return -1;
  }

  printf("Remote memory region: %p : %p, size %zd\n",
         (void*)ra.addr, (void*)(ra.addr + ra.extent), ra.extent);

  return 0;
}

uint64_t EbFtBase::_tryCq()
{
  static unsigned _iSrc = 0;

  // Cycle through all sources to find which one has data
  for (unsigned i = 0; i < _ep.size(); ++i)
  {
    unsigned iSrc = _iSrc++;
    if (_iSrc == _ep.size())  _iSrc = 0;

    if (!_ep[iSrc])  continue;

    int              cqNum;
    fi_cq_data_entry cqEntry;

    if (_ep[iSrc]->comp(&cqEntry, &cqNum, 1))
    {
      if (cqNum && (cqEntry.flags & (FI_REMOTE_WRITE | FI_REMOTE_CQ_DATA)))
      {
        // Revisit: Immediate data identifies which batch was written
        //          Better to use its address or parameters?
        //unsigned slot = (cqEntry.data >> 16) & 0xffff;
        //unsigned idx  =  cqEntry.data        & 0xffff;
        //batch = (Dgram*)&_pool[(slot * _maxBatches + idx) * _maxBatchSize];
        return _ra[iSrc].addr + cqEntry.data; // imm_data is only 32 bits for verbs!
      }
    }
    else
    {
      if (_ep[iSrc]->error_num() != -FI_EAGAIN)
      {
        fprintf(stderr, "Error completing operation with peer %u: %s\n",
                _id[iSrc], _ep[iSrc]->error());
      }
    }
    _ep[iSrc]->recv_comp_data();
  }

  return 0;
}

uint64_t EbFtBase::pend()
{
  uint64_t data = _tryCq();

  if (data)
  {
    //printf("EbFtBase::pend got data 0x%016lx\n", data);
    return data;
  }

  //printf("EbFtBase::Pending...\n");

  if (_cqPoller->poll())
  {
    data = _tryCq();
    //printf("EbFtBase::pend: poll woke with data 0x%016lx\n", data);
  }
  else
  {
    fprintf(stderr, "Error polling completion queues: %s\n",
            _cqPoller->error());
    return 0;
  }

  return data;
}

uint64_t EbFtBase::rmtAdx(unsigned dst, uint64_t offset)
{
  return _ra[_mappedId[dst]].addr + offset;
}

int EbFtBase::post(LocalIOVec& lclIov,
                   size_t      len,
                   unsigned    dst,
                   uint64_t    offset,
                   void*       ctx)
{
  //static unsigned wrtCnt  = 0;
  //static unsigned wrtCnt2 = 0;
  //static unsigned waitCnt = 0;

  //const struct iovec* iov = lclIov.iovecs();
  //void**              dsc = lclIov.desc();
  //
  //for (unsigned i = 0; i < lclIov.count(); ++i)
  //{
  //  printf("lclIov[%d]: base   = %p, size = %zd, desc = %p\n", i, iov[i].iov_base, iov[i].iov_len, dsc[i]);
  //}

  unsigned idx = _mappedId[dst];

  RemoteAddress rmtAdx(_ra[idx].rkey, _ra[idx].addr + offset, len);
  RemoteIOVec   rmtIov(&rmtAdx, 1);
  RmaMessage    rmaMsg(&lclIov, &rmtIov, ctx, offset); // imm_data is only 32 bits for verbs!

  //printf("rmtIov: rmtAdx = %p, size = %zd\n", (void*)rmtAdx.addr, len);

  //++wrtCnt;
  Endpoint* ep = _ep[idx];
  do
  {
    //++wrtCnt2;
    if (ep->writemsg(&rmaMsg, FI_REMOTE_CQ_DATA))  break;

    if (ep->error_num() == -FI_EAGAIN)
    {
      int              cqNum;
      fi_cq_data_entry cqEntry;

      //printf("EbFtBase::post: Waiting for comp... %d of %d, %d\n", ++waitCnt, wrtCnt, wrtCnt2);
      if (!ep->comp_wait(&cqEntry, &cqNum, 1))
      {
        if (ep->error_num() != -FI_EAGAIN)
        {
          fprintf(stderr, "Error completing operation with peer %u: %s\n",
                  idx, ep->error());
        }
      }
      ep->recv_comp_data();
    }
    else
    {
      fprintf(stderr, "writemsg failed: %s\n", ep->error());
      return ep->error_num();
    }
  }
  while (ep->state() == EP_CONNECTED);

  return 0;
}

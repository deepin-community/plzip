/* Plzip - Massively parallel implementation of lzip
   Copyright (C) 2009 Laszlo Ersek.
   Copyright (C) 2009-2022 Antonio Diaz Diaz.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#define _FILE_OFFSET_BITS 64

#include <algorithm>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <string>
#include <vector>
#include <stdint.h>
#include <unistd.h>
#include <lzlib.h>

#include "lzip.h"
#include "lzip_index.h"


namespace {

enum { max_packet_size = 1 << 20 };


struct Packet			// data block
  {
  uint8_t * data;		// data may be null if size == 0
  int size;			// number of bytes in data (if any)
  bool eom;			// end of member
  Packet() : data( 0 ), size( 0 ), eom( true ) {}
  Packet( uint8_t * const d, const int s, const bool e )
    : data( d ), size( s ), eom ( e ) {}
  ~Packet() { if( data ) delete[] data; }
  };


class Packet_courier			// moves packets around
  {
public:
  unsigned ocheck_counter;
  unsigned owait_counter;
private:
  int deliver_worker_id;	// worker queue currently delivering packets
  std::vector< std::queue< const Packet * > > opacket_queues;
  int num_working;			// number of workers still running
  const int num_workers;		// number of workers
  const unsigned out_slots;		// max output packets per queue
  pthread_mutex_t omutex;
  pthread_cond_t oav_or_exit;	// output packet available or all workers exited
  std::vector< pthread_cond_t > slot_av;	// output slot available
  const Shared_retval & shared_retval;		// discard new packets on error

  Packet_courier( const Packet_courier & );	// declared as private
  void operator=( const Packet_courier & );	// declared as private

public:
  Packet_courier( const Shared_retval & sh_ret, const int workers,
                  const int slots )
    : ocheck_counter( 0 ), owait_counter( 0 ), deliver_worker_id( 0 ),
      opacket_queues( workers ), num_working( workers ),
      num_workers( workers ), out_slots( slots ), slot_av( workers ),
      shared_retval( sh_ret )
    {
    xinit_mutex( &omutex ); xinit_cond( &oav_or_exit );
    for( unsigned i = 0; i < slot_av.size(); ++i ) xinit_cond( &slot_av[i] );
    }

  ~Packet_courier()
    {
    if( shared_retval() )		// cleanup to avoid memory leaks
      for( int i = 0; i < num_workers; ++i )
        while( !opacket_queues[i].empty() )
          { delete opacket_queues[i].front(); opacket_queues[i].pop(); }
    for( unsigned i = 0; i < slot_av.size(); ++i ) xdestroy_cond( &slot_av[i] );
    xdestroy_cond( &oav_or_exit ); xdestroy_mutex( &omutex );
    }

  void worker_finished()
    {
    // notify muxer when last worker exits
    xlock( &omutex );
    if( --num_working == 0 ) xsignal( &oav_or_exit );
    xunlock( &omutex );
    }

  // collect a packet from a worker, discard packet on error
  void collect_packet( const Packet * const opacket, const int worker_id )
    {
    xlock( &omutex );
    if( opacket->data )
      while( opacket_queues[worker_id].size() >= out_slots )
        {
        if( shared_retval() ) { delete opacket; goto done; }
        xwait( &slot_av[worker_id], &omutex );
        }
    opacket_queues[worker_id].push( opacket );
    if( worker_id == deliver_worker_id ) xsignal( &oav_or_exit );
done:
    xunlock( &omutex );
    }

  /* deliver a packet to muxer
     if packet->eom, move to next queue
     if packet data == 0, wait again */
  const Packet * deliver_packet()
    {
    const Packet * opacket = 0;
    xlock( &omutex );
    ++ocheck_counter;
    while( true )
      {
      while( opacket_queues[deliver_worker_id].empty() && num_working > 0 )
        {
        ++owait_counter;
        xwait( &oav_or_exit, &omutex );
        }
      if( opacket_queues[deliver_worker_id].empty() ) break;
      opacket = opacket_queues[deliver_worker_id].front();
      opacket_queues[deliver_worker_id].pop();
      if( opacket_queues[deliver_worker_id].size() + 1 == out_slots )
        xsignal( &slot_av[deliver_worker_id] );
      if( opacket->eom && ++deliver_worker_id >= num_workers )
        deliver_worker_id = 0;
      if( opacket->data ) break;
      delete opacket; opacket = 0;
      }
    xunlock( &omutex );
    return opacket;
    }

  bool finished()		// all packets delivered to muxer
    {
    if( num_working != 0 ) return false;
    for( int i = 0; i < num_workers; ++i )
      if( !opacket_queues[i].empty() ) return false;
    return true;
    }
  };


struct Worker_arg
  {
  const Lzip_index * lzip_index;
  Packet_courier * courier;
  const Pretty_print * pp;
  Shared_retval * shared_retval;
  int worker_id;
  int num_workers;
  int infd;
  };


/* Read members from file, decompress their contents, and give to courier
   the packets produced.
*/
extern "C" void * dworker_o( void * arg )
  {
  const Worker_arg & tmp = *(const Worker_arg *)arg;
  const Lzip_index & lzip_index = *tmp.lzip_index;
  Packet_courier & courier = *tmp.courier;
  const Pretty_print & pp = *tmp.pp;
  Shared_retval & shared_retval = *tmp.shared_retval;
  const int worker_id = tmp.worker_id;
  const int num_workers = tmp.num_workers;
  const int infd = tmp.infd;
  const int buffer_size = 65536;

  int new_pos = 0;
  uint8_t * new_data = 0;
  uint8_t * const ibuffer = new( std::nothrow ) uint8_t[buffer_size];
  LZ_Decoder * const decoder = LZ_decompress_open();
  if( !ibuffer || !decoder || LZ_decompress_errno( decoder ) != LZ_ok )
    { if( shared_retval.set_value( 1 ) ) { pp( mem_msg ); } goto done; }

  for( long i = worker_id; i < lzip_index.members(); i += num_workers )
    {
    long long member_pos = lzip_index.mblock( i ).pos();
    long long member_rest = lzip_index.mblock( i ).size();

    while( member_rest > 0 )
      {
      if( shared_retval() ) goto done;	// other worker found a problem
      while( LZ_decompress_write_size( decoder ) > 0 )
        {
        const int size = std::min( LZ_decompress_write_size( decoder ),
                    (int)std::min( (long long)buffer_size, member_rest ) );
        if( size > 0 )
          {
          if( preadblock( infd, ibuffer, size, member_pos ) != size )
            { if( shared_retval.set_value( 1 ) )
                { pp(); show_error( "Read error", errno ); } goto done; }
          member_pos += size;
          member_rest -= size;
          if( LZ_decompress_write( decoder, ibuffer, size ) != size )
            internal_error( "library error (LZ_decompress_write)." );
          }
        if( member_rest <= 0 ) { LZ_decompress_finish( decoder ); break; }
        }
      while( true )			// read and pack decompressed data
        {
        if( !new_data &&
            !( new_data = new( std::nothrow ) uint8_t[max_packet_size] ) )
          { if( shared_retval.set_value( 1 ) ) { pp( mem_msg ); } goto done; }
        const int rd = LZ_decompress_read( decoder, new_data + new_pos,
                                           max_packet_size - new_pos );
        if( rd < 0 )
          { decompress_error( decoder, pp, shared_retval, worker_id );
            goto done; }
        new_pos += rd;
        if( new_pos > max_packet_size )
          internal_error( "opacket size exceeded in worker." );
        const bool eom = LZ_decompress_finished( decoder ) == 1;
        if( new_pos == max_packet_size || eom )		// make data packet
          {
          const Packet * const opacket =
            new Packet( ( new_pos > 0 ) ? new_data : 0, new_pos, eom );
          courier.collect_packet( opacket, worker_id );
          if( new_pos > 0 ) { new_pos = 0; new_data = 0; }
          if( eom )
            { LZ_decompress_reset( decoder );	// prepare for new member
              break; }
          }
        if( rd == 0 ) break;
        }
      }
    show_progress( lzip_index.mblock( i ).size() );
    }
done:
  delete[] ibuffer; if( new_data ) delete[] new_data;
  if( LZ_decompress_member_position( decoder ) != 0 &&
      shared_retval.set_value( 1 ) )
    pp( "Error, some data remains in decoder." );
  if( LZ_decompress_close( decoder ) < 0 && shared_retval.set_value( 1 ) )
    pp( "LZ_decompress_close failed." );
  courier.worker_finished();
  return 0;
  }


/* Get from courier the processed and sorted packets, and write their
   contents to the output file. Drain queue on error.
*/
void muxer( Packet_courier & courier, const Pretty_print & pp,
            Shared_retval & shared_retval, const int outfd )
  {
  while( true )
    {
    const Packet * const opacket = courier.deliver_packet();
    if( !opacket ) break;	// queue is empty. all workers exited

    if( shared_retval() == 0 &&
        writeblock( outfd, opacket->data, opacket->size ) != opacket->size &&
        shared_retval.set_value( 1 ) )
      { pp(); show_error( "Write error", errno ); }
    delete opacket;
    }
  }

} // end namespace


// init the courier, then start the workers and call the muxer.
int dec_stdout( const int num_workers, const int infd, const int outfd,
                const Pretty_print & pp, const int debug_level,
                const int out_slots, const Lzip_index & lzip_index )
  {
  Shared_retval shared_retval;
  Packet_courier courier( shared_retval, num_workers, out_slots );

  Worker_arg * worker_args = new( std::nothrow ) Worker_arg[num_workers];
  pthread_t * worker_threads = new( std::nothrow ) pthread_t[num_workers];
  if( !worker_args || !worker_threads )
    { pp( mem_msg ); delete[] worker_threads; delete[] worker_args; return 1; }

  int i = 0;				// number of workers started
  for( ; i < num_workers; ++i )
    {
    worker_args[i].lzip_index = &lzip_index;
    worker_args[i].courier = &courier;
    worker_args[i].pp = &pp;
    worker_args[i].shared_retval = &shared_retval;
    worker_args[i].worker_id = i;
    worker_args[i].num_workers = num_workers;
    worker_args[i].infd = infd;
    const int errcode =
      pthread_create( &worker_threads[i], 0, dworker_o, &worker_args[i] );
    if( errcode )
      { if( shared_retval.set_value( 1 ) )
          { show_error( "Can't create worker threads", errcode ); } break; }
    }

  muxer( courier, pp, shared_retval, outfd );

  while( --i >= 0 )
    {
    const int errcode = pthread_join( worker_threads[i], 0 );
    if( errcode && shared_retval.set_value( 1 ) )
      show_error( "Can't join worker threads", errcode );
    }
  delete[] worker_threads;
  delete[] worker_args;

  if( shared_retval() ) return shared_retval();	// some thread found a problem

  if( verbosity >= 1 )
    show_results( lzip_index.cdata_size(), lzip_index.udata_size(),
                  lzip_index.dictionary_size(), false );

  if( debug_level & 1 )
    std::fprintf( stderr,
      "workers started                           %8u\n"
      "muxer tried to consume from workers       %8u times\n"
      "muxer had to wait                         %8u times\n",
      num_workers, courier.ocheck_counter, courier.owait_counter );

  if( !courier.finished() ) internal_error( "courier not finished." );
  return 0;
  }

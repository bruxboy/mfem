// Copyright (c) 2010, Lawrence Livermore National Security, LLC. Produced at
// the Lawrence Livermore National Laboratory. LLNL-CODE-443211. All Rights
// reserved. See file COPYRIGHT for details.
//
// This file is part of the MFEM library. For more information and source code
// availability see http://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the GNU Lesser General Public License (as published by the Free
// Software Foundation) version 2.1 dated February 1999.

#ifndef MFEM_COMMUNICATION
#define MFEM_COMMUNICATION

#include "../config/config.hpp"

#ifdef MFEM_USE_MPI

#include "array.hpp"
#include "table.hpp"
#include "sets.hpp"
#include "globals.hpp"
#include <mpi.h>


namespace mfem
{

/** @brief A simple convenience class that calls MPI_Init() at construction and
    MPI_Finalize() at destruction. It also provides easy access to
    MPI_COMM_WORLD's rank and size. */
class MPI_Session
{
protected:
   int world_rank, world_size;
   void GetRankAndSize();
public:
   MPI_Session() { MPI_Init(NULL, NULL); GetRankAndSize(); }
   MPI_Session(int &argc, char **&argv)
   { MPI_Init(&argc, &argv); GetRankAndSize(); }
   ~MPI_Session() { MPI_Finalize(); }
   /// Return MPI_COMM_WORLD's rank.
   int WorldRank() const { return world_rank; }
   /// Return MPI_COMM_WORLD's size.
   int WorldSize() const { return world_size; }
   /// Return true if WorldRank() == 0.
   bool Root() const { return world_rank == 0; }
};

class GroupTopology
{
private:
   MPI_Comm   MyComm;

   /* The shared entities (e.g. vertices, faces and edges) are split into
      groups, each group determined by the set of participating processors.
      They are numbered locally in lproc. Assumptions:
      - group 0 is the 'local' group
      - groupmaster_lproc[0] = 0
      - lproc_proc[0] = MyRank */

   // Neighbor ids (lproc) in each group.
   Table      group_lproc;
   // Master neighbor id for each group.
   Array<int> groupmaster_lproc;
   // MPI rank of each neighbor.
   Array<int> lproc_proc;
   // Group --> Group number in the master.
   Array<int> group_mgroup;

   void ProcToLProc();

public:
   GroupTopology() : MyComm(0) {}
   GroupTopology(MPI_Comm comm) { MyComm = comm; }

   /// Copy constructor
   GroupTopology(const GroupTopology &gt);
   void SetComm(MPI_Comm comm) { MyComm = comm; }

   MPI_Comm GetComm() const { return MyComm; }
   int MyRank() const { int r; MPI_Comm_rank(MyComm, &r); return r; }
   int NRanks() const { int s; MPI_Comm_size(MyComm, &s); return s; }

   void Create(ListOfIntegerSets &groups, int mpitag);

   int NGroups() const { return group_lproc.Size(); }
   // return the number of neighbors including the local processor
   int GetNumNeighbors() const { return lproc_proc.Size(); }
   int GetNeighborRank(int i) const { return lproc_proc[i]; }
   // am I master for group 'g'?
   bool IAmMaster(int g) const { return (groupmaster_lproc[g] == 0); }
   // return the neighbor index of the group master for a given group.
   // neighbor 0 is the local processor
   int GetGroupMaster(int g) const { return groupmaster_lproc[g]; }
   // return the rank of the group master for a given group
   int GetGroupMasterRank(int g) const
   { return lproc_proc[groupmaster_lproc[g]]; }
   // for a given group return the group number in the master
   int GetGroupMasterGroup(int g) const { return group_mgroup[g]; }
   // get the number of processors in a group
   int GetGroupSize(int g) const { return group_lproc.RowSize(g); }
   // return a pointer to a list of neighbors for a given group.
   // neighbor 0 is the local processor
   const int *GetGroup(int g) const { return group_lproc.GetRow(g); }

   /// Save the data in a stream.
   void Save(std::ostream &out) const;
   /// Load the data from a stream.
   void Load(std::istream &in);

   virtual ~GroupTopology() {}
};

/** @brief Communicator performing operations within groups defined by a
    GroupTopology with arbitrary-size data associated with each group. */
class GroupCommunicator
{
public:
   /// Communication mode.
   enum Mode
   {
      byGroup,    ///< Communications are performed one group at a time.
      byNeighbor  /**< Communications are performed one neighbor at a time,
                       aggregating over groups. */
   };

private:
   GroupTopology &gtopo;
   Mode mode;
   Table group_ldof;
   Table group_ltdof; // only for groups for which this processor is master.
   int group_buf_size;
   Array<char> group_buf;
   MPI_Request *requests;
   // MPI_Status  *statuses;
   int comm_lock; // 0 - no lock, 1 - locked for Bcast, 2 - locked for Reduce
   int num_requests;
   int *request_marker;
   int *buf_offsets; // size = max(number of groups, number of neighbors)
   Table nbr_send_groups, nbr_recv_groups; // nbr 0 = me

public:
   /// Construct a GroupCommunicator object.
   /** The object must be initialized before it can be used to perform any
       operations. To initialize the object, either
       - call Create() or
       - initialize the Table reference returned by GroupLDofTable() and then
         call Finalize().
   */
   GroupCommunicator(GroupTopology &gt, Mode m = byNeighbor);

   /** @brief Initialize the communicator from a local-dof to group map.
       Finalize() is called internally. */
   void Create(const Array<int> &ldof_group);

   /** @brief Fill-in the returned Table reference to initialize the
       GroupCommunicator then call Finalize(). */
   Table &GroupLDofTable() { return group_ldof; }

   /// Allocate internal buffers after the GroupLDofTable is defined
   void Finalize();

   /// Initialize the internal group_ltdof Table.
   /** This method must be called before performing operations that use local
       data layout 2, see CopyGroupToBuffer() for layout descriptions. */
   void SetLTDofTable(const Array<int> &ldof_ltdof);

   /// Get a reference to the associated GroupTopology object
   GroupTopology &GetGroupTopology() { return gtopo; }

   /** @brief Data structure on which we define reduce operations.

     The data is associated with (and the operation is performed on) one group
     at a time. */
   template <class T> struct OpData
   {
      int nldofs, nb, *ldofs;
      T *ldata, *buf;
   };

   /** @brief Copy the entries corresponding to the group @a group from the
       local array @a ldata to the buffer @a buf. */
   /** The @a layout of the local array can be:
       - 0 - @a ldata is an array on all ldofs: copied indices:
         `{ J[j] : I[group] <= j < I[group+1] }` where `I,J=group_ldof.{I,J}`
       - 1 - @a ldata is an array on the shared ldofs: copied indices:
         `{ j : I[group] <= j < I[group+1] }` where `I,J=group_ldof.{I,J}`
       - 2 - @a ldata is an array on the true ldofs, ltdofs: copied indices:
         `{ J[j] : I[group] <= j < I[group+1] }` where `I,J=group_ltdof.{I,J}`.
       @returns The pointer @a buf plus the number of elements in the group. */
   template <class T>
   T *CopyGroupToBuffer(const T *ldata, T *buf, int group, int layout) const;

   /** @brief Copy the entries corresponding to the group @a group from the
       buffer @a buf to the local array @a ldata. */
   /** For a description of @a layout, see CopyGroupToBuffer().
       @returns The pointer @a buf plus the number of elements in the group. */
   template <class T>
   const T *CopyGroupFromBuffer(const T *buf, T *ldata, int group,
                                int layout) const;

   /** @brief Perform the reduction operation @a Op to the entries of group
       @a group using the values from the buffer @a buf and the values from the
       local array @a ldata, saving the result in the latter. */
   /** For a description of @a layout, see CopyGroupToBuffer().
       @returns The pointer @a buf plus the number of elements in the group. */
   template <class T>
   const T *ReduceGroupFromBuffer(const T *buf, T *ldata, int group,
                                  int layout, void (*Op)(OpData<T>)) const;

   /// Begin a broadcast within each group where the master is the root.
   /** For a description of @a layout, see CopyGroupToBuffer(). */
   template <class T> void BcastBegin(T *ldata, int layout);

   /** @brief Finalize a broadcast started with BcastBegin().

       The output data @a layout can be:
       - 0 - @a ldata is an array on all ldofs; the input layout should be
             either 0 or 2
       - 1 - @a ldata is the same array as given to BcastBegin(); the input
             layout should be 1.

       For more details about @a layout, see CopyGroupToBuffer(). */
   template <class T> void BcastEnd(T *ldata, int layout);

   /** @brief Broadcast within each group where the master is the root.

       The data @a layout can be either 0 or 1.

       For a description of @a layout, see CopyGroupToBuffer(). */
   template <class T> void Bcast(T *ldata, int layout)
   {
      BcastBegin(ldata, layout);
      BcastEnd(ldata, layout);
   }

   /// Broadcast within each group where the master is the root.
   template <class T> void Bcast(T *ldata) { Bcast<T>(ldata, 0); }
   /// Broadcast within each group where the master is the root.
   template <class T> void Bcast(Array<T> &ldata) { Bcast<T>((T *)ldata); }

   /** @brief Begin reduction operation within each group where the master is
       the root. */
   /** The input data layout is an array on all ldofs, i.e. layout 0, see
       CopyGroupToBuffer().

       The reduce operation will be specified when calling ReduceEnd(). This
       method is instantiated for int and double. */
   template <class T> void ReduceBegin(const T *ldata);

   /** @brief Finalize reduction operation started with ReduceBegin().

       The output data @a layout can be either 0 or 2, see CopyGroupToBuffer().

       The reduce operation is given by the third argument (see below for list
       of the supported operations.) This method is instantiated for int and
       double.

       @note If the output data layout is 2, then the data from the @a ldata
       array passed to this call is used in the reduction operation, instead of
       the data from the @a ldata array passed to ReduceBegin(). Therefore, the
       data for master-groups has to be identical in both arrays.
   */
   template <class T> void ReduceEnd(T *ldata, int layout,
                                     void (*Op)(OpData<T>));

   /** @brief Reduce within each group where the master is the root.

       The reduce operation is given by the second argument (see below for list
       of the supported operations.) */
   template <class T> void Reduce(T *ldata, void (*Op)(OpData<T>))
   {
      ReduceBegin(ldata);
      ReduceEnd(ldata, 0, Op);
   }

   /// Reduce within each group where the master is the root.
   template <class T> void Reduce(Array<T> &ldata, void (*Op)(OpData<T>))
   { Reduce<T>((T *)ldata, Op); }

   /// Reduce operation Sum, instantiated for int and double
   template <class T> static void Sum(OpData<T>);
   /// Reduce operation Min, instantiated for int and double
   template <class T> static void Min(OpData<T>);
   /// Reduce operation Max, instantiated for int and double
   template <class T> static void Max(OpData<T>);
   /// Reduce operation bitwise OR, instantiated for int only
   template <class T> static void BitOR(OpData<T>);

   /// Print information about the GroupCommunicator from all MPI ranks.
   void PrintInfo(std::ostream &out = mfem::out) const;

   /** @brief Destroy a GroupCommunicator object, deallocating internal data
       structures and buffers. */
   ~GroupCommunicator();
};


/// \brief Variable-length MPI message containing unspecific binary data.
template<int Tag>
struct VarMessage
{
   std::string data;
   MPI_Request send_request;

   /// Non-blocking send to processor 'rank'.
   void Isend(int rank, MPI_Comm comm)
   {
      Encode(rank);
      MPI_Isend((void*) data.data(), data.length(), MPI_BYTE, rank, Tag, comm,
                &send_request);
   }

   /// Helper to send all messages in a rank-to-message map container.
   template<typename MapT>
   static void IsendAll(MapT& rank_msg, MPI_Comm comm)
   {
      typename MapT::iterator it;
      for (it = rank_msg.begin(); it != rank_msg.end(); ++it)
      {
         it->second.Isend(it->first, comm);
      }
   }

   /// Helper to wait for all messages in a map container to be sent.
   template<typename MapT>
   static void WaitAllSent(MapT& rank_msg)
   {
      typename MapT::iterator it;
      for (it = rank_msg.begin(); it != rank_msg.end(); ++it)
      {
         MPI_Wait(&it->second.send_request, MPI_STATUS_IGNORE);
         it->second.Clear();
      }
   }

   /** Blocking probe for incoming message of this type from any rank.
       Returns the rank and message size. */
   static void Probe(int &rank, int &size, MPI_Comm comm)
   {
      MPI_Status status;
      MPI_Probe(MPI_ANY_SOURCE, Tag, comm, &status);
      rank = status.MPI_SOURCE;
      MPI_Get_count(&status, MPI_BYTE, &size);
   }

   /** Non-blocking probe for incoming message of this type from any rank.
       If there is an incoming message, returns true and sets 'rank' and 'size'.
       Otherwise returns false. */
   static bool IProbe(int &rank, int &size, MPI_Comm comm)
   {
      int flag;
      MPI_Status status;
      MPI_Iprobe(MPI_ANY_SOURCE, Tag, comm, &flag, &status);
      if (!flag) { return false; }

      rank = status.MPI_SOURCE;
      MPI_Get_count(&status, MPI_BYTE, &size);
      return true;
   }

   /// Post-probe receive from processor 'rank' of message size 'size'.
   void Recv(int rank, int size, MPI_Comm comm)
   {
      MFEM_ASSERT(size >= 0, "");
      data.resize(size);
      MPI_Status status;
      MPI_Recv((void*) data.data(), size, MPI_BYTE, rank, Tag, comm, &status);
#ifdef MFEM_DEBUG
      int count;
      MPI_Get_count(&status, MPI_BYTE, &count);
      MFEM_VERIFY(count == size, "");
#endif
      Decode(rank);
   }

   /// Like Recv(), but throw away the messsage.
   void RecvDrop(int rank, int size, MPI_Comm comm)
   {
      data.resize(size);
      MPI_Status status;
      MPI_Recv((void*) data.data(), size, MPI_BYTE, rank, Tag, comm, &status);
      data.resize(0); // don't decode
   }

   /// Helper to receive all messages in a rank-to-message map container.
   template<typename MapT>
   static void RecvAll(MapT& rank_msg, MPI_Comm comm)
   {
      int recv_left = rank_msg.size();
      while (recv_left > 0)
      {
         int rank, size;
         Probe(rank, size, comm);
         MFEM_ASSERT(rank_msg.find(rank) != rank_msg.end(), "Unexpected message"
                     " (tag " << Tag << ") from rank " << rank);
         // NOTE: no guard against receiving two messages from the same rank
         rank_msg[rank].Recv(rank, size, comm);
         --recv_left;
      }
   }

   VarMessage() : send_request(MPI_REQUEST_NULL) {}
   void Clear() { data.clear(); send_request = MPI_REQUEST_NULL; }

   virtual ~VarMessage()
   {
      MFEM_ASSERT(send_request == MPI_REQUEST_NULL,
                  "WaitAllSent was not called after Isend");
   }

   VarMessage(const VarMessage &other)
      : data(other.data), send_request(other.send_request)
   {
      MFEM_ASSERT(send_request == MPI_REQUEST_NULL,
                  "Cannot copy message with a pending send.");
   }

protected:
   virtual void Encode(int rank) {}
   virtual void Decode(int rank) {}
};


/// Helper struct to convert a C++ type to an MPI type
template <typename Type> struct MPITypeMap;

// Specializations of MPITypeMap; mpi_type initialized in communication.cpp:
template<> struct MPITypeMap<int>    { static const MPI_Datatype mpi_type; };
template<> struct MPITypeMap<double> { static const MPI_Datatype mpi_type; };


/** Reorder MPI ranks to follow the Z-curve within the physical machine topology
    (provided that functions to query physical node coordinates are available).
    Returns a new communicator with reordered ranks. */
MPI_Comm ReorderRanksZCurve(MPI_Comm comm);


} // namespace mfem

#endif

#endif

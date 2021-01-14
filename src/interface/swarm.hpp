//========================================================================================
// (C) (or copyright) 2020. Triad National Security, LLC. All rights reserved.
//
// This program was produced under U.S. Government contract 89233218CNA000001 for Los
// Alamos National Laboratory (LANL), which is operated by Triad National Security, LLC
// for the U.S. Department of Energy/National Nuclear Security Administration. All rights
// in the program are reserved by Triad National Security, LLC, and the U.S. Department
// of Energy/National Nuclear Security Administration. The Government is granted for
// itself and others acting on its behalf a nonexclusive, paid-up, irrevocable worldwide
// license in this material to reproduce, prepare derivative works, distribute copies to
// the public, perform publicly and display publicly, and to permit others to do so.
//========================================================================================
#ifndef INTERFACE_SWARM_HPP_
#define INTERFACE_SWARM_HPP_

///
/// A swarm contains all particles of a particular species
/// Date: August 21, 2019

#include <array>
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "basic_types.hpp"
#include "bvals/swarm/bvals_swarm.hpp"
#include "globals.hpp" // my_rank
#include "metadata.hpp"
#include "parthenon_arrays.hpp"
#include "parthenon_mpi.hpp"
#include "variable.hpp"
#include "variable_pack.hpp"

namespace parthenon {
class MeshBlock;

enum class PARTICLE_STATUS { UNALLOCATED, ALIVE, DEAD };

class SwarmDeviceContext {
 public:
  KOKKOS_FUNCTION
  bool IsActive(int n) const { return mask_(n); }

  KOKKOS_FUNCTION
  bool IsOnCurrentMeshBlock(int n) const { return blockIndex_(n) == this_block_; }

  KOKKOS_FUNCTION
  void MarkParticleForRemoval(int n) const { marked_for_removal_(n) = true; }

  KOKKOS_FUNCTION
  bool IsMarkedForRemoval(const int n) const { return marked_for_removal_(n); }

  KOKKOS_INLINE_FUNCTION
  int GetNeighborBlockIndex(const int &n, const double &x, const double &y,
                            const double &z) const {

    int i = static_cast<int>((x - x_min_) / ((x_max_ - x_min_) / 2.)) + 1;
    int j = static_cast<int>((y - y_min_) / ((y_max_ - y_min_) / 2.)) + 1;
    int k = static_cast<int>((z - z_min_) / ((z_max_ - z_min_) / 2.)) + 1;

    // Something went wrong
    if (i < 0 || i > 3 || ((j < 0 || j > 3) && ndim_ > 1) ||
        ((k < 0 || k > 3) && ndim_ > 2)) {
      blockIndex_(n) = -2;
      // TODO(BRR) not sure how to handle this case... which hopefully never happens once
      // everything is working
      PARTHENON_FAIL("Particle neighbor indices out of bounds");
      return -2;
    }

    // Ignore k,j indices as necessary based on problem dimension
    if (ndim_ == 1) {
      int i = static_cast<int>((x - x_min_) / ((x_max_ - x_min_) / 2.)) + 1;
      blockIndex_(n) = neighborIndices_(0, 0, i);
    } else if (ndim_ == 2) {
      int i = static_cast<int>((x - x_min_) / ((x_max_ - x_min_) / 2.)) + 1;
      int j = static_cast<int>((y - y_min_) / ((y_max_ - y_min_) / 2.)) + 1;
      blockIndex_(n) = neighborIndices_(0, j, i);
    } else {
      int i = static_cast<int>((x - x_min_) / ((x_max_ - x_min_) / 2.)) + 1;
      int j = static_cast<int>((y - y_min_) / ((y_max_ - y_min_) / 2.)) + 1;
      int k = static_cast<int>((z - z_min_) / ((z_max_ - z_min_) / 2.)) + 1;
      blockIndex_(n) = neighborIndices_(k, j, i);
    }

    return blockIndex_(n);
  }


 private:
  Real x_min_;
  Real x_max_;
  Real y_min_;
  Real y_max_;
  Real z_min_;
  Real z_max_;
  Real x_min_global_;
  Real x_max_global_;
  Real y_min_global_;
  Real y_max_global_;
  Real z_min_global_;
  Real z_max_global_;
  ParArrayND<bool> marked_for_removal_;
  ParArrayND<bool> mask_;
  ParArrayND<int> blockIndex_;
  ParArrayND<int> neighbor_send_index_; // TODO(BRR) is this needed?
  ParArrayND<int> neighborIndices_;     // 4x4x4 array of possible block AMR regions
  int ndim_;
  friend class Swarm;
  constexpr static int this_block_ = -1; // Mirrors definition in Swarm class
};

class Swarm {
 public:
  Swarm(const std::string &label, const Metadata &metadata, const int nmax_pool_in = 3);

  /// Returns shared pointer to a block
  std::shared_ptr<MeshBlock> GetBlockPointer() const {
    if (pmy_block.expired()) {
      PARTHENON_THROW("Invalid pointer to MeshBlock!");
    }
    return pmy_block.lock();
  }

  SwarmDeviceContext GetDeviceContext() const;

  // Set the pointer to the mesh block for this swarm
  void SetBlockPointer(std::weak_ptr<MeshBlock> pmb) { pmy_block = pmb; }

  /// Make a new Swarm based on an existing one
  std::shared_ptr<Swarm> AllocateCopy(const bool allocComms = false,
                                      MeshBlock *pmb = nullptr);

  /// Add variable to swarm
  void Add(const std::string &label, const Metadata &metadata);

  /// Add multiple variables with common metadata to swarm
  void Add(const std::vector<std::string> &labelVector, const Metadata &metadata);

  /// Remote a variable from swarm
  void Remove(const std::string &label);

  /// Get real particle variable
  ParticleVariable<Real> &GetReal(const std::string &label) {
    return *(realMap_.at(label));
  }

  /// Get integer particle variable
  ParticleVariable<int> &GetInteger(const std::string &label) {
    return *(intMap_.at(label));
  }

  /// Assign label for swarm
  void setLabel(const std::string &label) { label_ = label; }

  /// retrieve label for swarm
  std::string label() const { return label_; }

  /// retrieve metadata for swarm
  const Metadata metadata() const { return m_; }

  /// Assign info for swarm
  void setInfo(const std::string &info) { info_ = info; }

  /// return information string
  std::string info() const { return info_; }

  /// Expand pool size geometrically as necessary
  void increasePoolMax() { setPoolMax(2 * nmax_pool_); }

  /// Set max pool size
  void setPoolMax(const int nmax_pool);

  /// Check whether metadata bit is set
  bool IsSet(const MetadataFlag bit) const { return m_.IsSet(bit); }

  /// Get the last index of active particles
  int get_max_active_index() const { return max_active_index_; }

  /// Get number of active particles
  int get_num_active() const { return num_active_; }

  /// Get the quality of the data layout. 1 is perfectly organized, < 1
  /// indicates gaps in the list.
  Real get_packing_efficiency() const { return num_active_ / (max_active_index_ + 1); }

  /// Remove particles marked for removal and update internal indexing
  void RemoveMarkedParticles();

  /// Open up memory for new empty particles, return a mask to these particles
  ParArrayND<bool> AddEmptyParticles(const int num_to_add, ParArrayND<int> &new_indices);

  /// Defragment the list by moving active particles so they are contiguous in
  /// memory
  void Defrag();

  // used in case of swarm boundary communication
  void SetupPersistentMPI();
  std::shared_ptr<BoundarySwarm> vbvar;
  bool mpiStatus;
  void allocateComms(std::weak_ptr<MeshBlock> wpmb);

  int GetParticleDataSize() { return realVector_.size() + intVector_.size(); }

  bool Send(BoundaryCommSubset phase);

  bool Receive(BoundaryCommSubset phase);

  // VariablePack<Real> PackRealVariables();
  // VariablePack<int> PackIntVariables();
  //  template <typename T>
  // VariablePack<T> PackVariables(const std::vector<std::string> &names,
  //                                 const vpack_types::VarList<T> &vars,
  //                               PackIndexMap &vmap);
  vpack_types::SwarmVarList<Real> MakeRealList_(std::vector<std::string> &names);
  vpack_types::SwarmVarList<int> MakeIntList_(std::vector<std::string> &names);

  SwarmVariablePack<Real> PackVariablesReal(
      const std::vector<std::string> &names,
      //                                     const vpack_types::VarList<Real> &vars,
      PackIndexMap &vmap);
  SwarmVariablePack<Real> PackAllVariablesReal(PackIndexMap &vmap);
  SwarmVariablePack<int> PackVariablesInt(
      const std::vector<std::string> &names,
      //                                     const vpack_types::VarList<int> &vars,
      PackIndexMap &vmap);

  void PackAllVariables(SwarmVariablePack<Real> &vreal, SwarmVariablePack<int> &vint);
  void PackAllVariables(SwarmVariablePack<Real> &vreal, SwarmVariablePack<int> &vint,
    PackIndexMap &rmap, PackIndexMap &imap);

  bool StartCommunication(BoundaryCommSubset phase) {
    printf("[%i] StartCommunication!\n", Globals::my_rank);
    mpiStatus = false;

    global_num_incomplete_ = 3;
    local_num_completed_ = 0;

#ifdef MPI_PARALLEL
    MPI_Allreduce(MPI_IN_PLACE, &global_num_incomplete_, 1, MPI_INT, MPI_SUM,
                  MPI_COMM_WORLD);
#endif

    printf("global_num_incomplete_: %i\n", global_num_incomplete_);

    vbvar->StartReceiving(phase);

    return true;
  }
  bool SillyUpdate() {
    printf("[%i] SillyUpdate!\n", Globals::my_rank);
    if (mpiStatus == true) {
      return true;
    }

    // vbvar->Receive();

    local_num_completed_ += 1;

    return false;
  }
  bool FinishCommunication(BoundaryCommSubset phase);

  // Temporarily public
  int swarm_num_incomplete_;
  int global_num_incomplete_;
  int local_num_completed_;
  int global_num_completed_;
  MPI_Request allreduce_request_;
  int num_particles_sent_;
  bool finished_transport;

 private:
  int debug = 0;
  std::weak_ptr<MeshBlock> pmy_block;

  int nmax_pool_;
  int max_active_index_ = 0;
  int num_active_ = 0;
  Metadata m_;
  std::string label_;
  std::string info_;
  std::shared_ptr<ParArrayND<PARTICLE_STATUS>> pstatus_;
  ParticleVariableVector<int> intVector_;
  ParticleVariableVector<Real> realVector_;

  MapToParticle<int> intMap_;
  MapToParticle<Real> realMap_;

  std::list<int> free_indices_;
  ParticleVariable<bool> mask_;
  ParticleVariable<bool> marked_for_removal_;
  ParticleVariable<int> neighbor_send_index_; // -1 means no send
  ParArrayND<int> neighborIndices_; // Indexing of vbvar's neighbor array. -1 for same.
                                    // k,j indices unused in 3D&2D, 2D, respectively
  ParArrayND<int> blockIndex_; // Neighbor index for each particle. -1 for current block.

  template <typename T>
  void ResizeParArray(ParArrayND<T> &var, int n_old, int n_new);
  //MapToSwarmVariablePack<Real> varPackMapReal_;
  //MapToSwarmVariablePack<int> varPackMapInt_;

  constexpr static int this_block_ = -1;
  constexpr static int unset_index_ = -1;
};

using SP_Swarm = std::shared_ptr<Swarm>;
using SwarmVector = std::vector<SP_Swarm>;
using SwarmMap = std::unordered_map<std::string, SP_Swarm>;

} // namespace parthenon

#endif // INTERFACE_SWARM_HPP_

#ifndef TClusterSlice_hxx_seen
#define TClusterSlice_hxx_seen
#include <TAlgorithm.hxx>
#include <TAlgorithmResult.hxx>

namespace CP {
    class TClusterSlice;
};

/// This takes a algorithm result with clusters of "simply connected" hits
/// (meaning hits that are connected by direct neighbors), and then splits
/// each cluster into clusters in "Z".  The resulting clusters are then turned
/// into tracks.  This means that 3D hits which are part of the same XY plane
/// confusion region are all in the same cluster.  The track is then assumed
/// to go through the center of each cluster.  This is intended as a first
/// approximation to atrack.
class CP::TClusterSlice
    : public CP::TAlgorithm {
public:
    TClusterSlice();
    virtual ~TClusterSlice();

    /// Apply the algorithm.
    CP::THandle<CP::TAlgorithmResult> 
    Process(const CP::TAlgorithmResult& input,
            const CP::TAlgorithmResult& input1 = CP::TAlgorithmResult::Empty,
            const CP::TAlgorithmResult& input2 = CP::TAlgorithmResult::Empty);

private:
    /// Take a single cluster of simply connected hits and turn it into a
    /// track.  If the hits are not long enough to turn into a track, then
    /// the original cluster is returned.  If the input object is not a
    /// cluster, then the original object is returned.
    CP::THandle<CP::TReconBase> 
    ClusterToTrack(CP::THandle<CP::TReconBase> input);

    /// Take a single cluster and break it into clusters based on the "Z
    /// Confusion Distance".
    CP::THandle<CP::TReconObjectContainer> 
    BreakCluster(CP::THandle<CP::TReconBase> input);

    /// The minimum number of neighbors within the maximum distance of the
    /// current point to consider the current point to be in a high density
    /// region.
    int fMinPoints;

    /// The radius over which points are counted to determine if a point is in
    /// a high density region.
    int fMaxDist;

    /// The minimum number of hits before a cluster is broken up.  This keeps
    /// "micro" clusters from being split up since they will be best handled
    /// using the cluster directly.
    unsigned int fMinHits;

    /// The splitting distance in Z.
    double fClusterStep;
};
#endif

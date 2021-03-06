#include "TLinearRoad.hxx"
#include "TClusterMomentsFit.hxx"
#include "HitUtilities.hxx"
#include "CreateTrack.hxx"

#include <ostreamTLorentzVector.hxx>
#include <ostreamTVector3.hxx>

#include <HEPUnits.hxx>

#include <TVector3.h>
#include <TPrincipal.h>

#include <numeric>
#include <cmath>
#include <algorithm>
#include <string>
#include <memory>

bool 
CheckUniqueInternal(std::vector< CP::THandle<CP::TReconCluster> >& vec) {
    std::sort(vec.begin(),vec.end());
    std::vector< CP::THandle<CP::TReconCluster> >::iterator 
        end = std::unique(vec.begin(), vec.end());
    if (end != vec.end()) {
        CaptLog("Duplicate objects in container");
        return false;
    }
    return true;
}

template <typename iterator>
bool CheckUnique(iterator begin, iterator end) {
    std::vector< CP::THandle<CP::TReconCluster> > vec;
    std::copy(begin, end, std::back_inserter(vec));
    if (!CheckUniqueInternal(vec)) {
        for (iterator i = begin; i != end; ++i) {
            CaptLog("   " << CP::GetPointer(*i));
        }
        return false;
    }
    return true;
}

CP::TLinearRoad::TLinearRoad(int maxClusters)
    : fMaxClusters(maxClusters),
      fRoadWidth(12*unit::mm), 
      fRoadStep(5.0*unit::cm),
      fOpeningAngle(0.15*unit::radian),
      fSeedSize(10),
      fSeedLength(5.0*unit::cm) { }

void CP::TLinearRoad::FillRemains(CP::TReconObjectContainer& remains) const {
    remains.clear();
    std::copy(fRemainingClusters.begin(), fRemainingClusters.end(),
              std::back_inserter(remains));
}

void CP::TLinearRoad::Process(const CP::TReconObjectContainer& seed,
                              const CP::TReconObjectContainer& clusters) {

    CaptLog("Follow road from " << seed.size() << " cluster seed"
            << " with " << clusters.size() << " remaining clusters.");

    // Check the inputs and internal structures to make sure things are OK.
    if (!fRemainingClusters.empty()) {
        CaptError("Remaining clusters not empty.");
        std::abort();
    }

    if (!fOriginalClusters.empty()) {
        CaptError("Original clusters not empty.");
        std::abort();
    }

    if (!fTrackClusters.empty()) {
        CaptError("Track clusters not empty.");
        std::abort();
    }

    // Copy the input clusters and make sure they really are all clusters.
    fRemainingClusters.clear();
    for (CP::TReconObjectContainer::const_iterator c = clusters.begin();
         c != clusters.end(); ++c) {
        CP::THandle<CP::TReconCluster> cluster = *c;
        if (!cluster) {
            CaptError("Non-cluster in input objects");
            std::abort();
        }
        fRemainingClusters.push_back(cluster);
    }
    
    // Copy the seed and make sure it's really all clusters.  This keeps two
    // copies, one (fTrackClusters) will be expanded with new hits, and the
    // other is to keep track of the original seed clusters.
    for (CP::TReconObjectContainer::const_iterator c = seed.begin();
         c != seed.end(); ++c) {
        CP::THandle<CP::TReconCluster> cluster = *c;
        if (!cluster) {
            CaptError("Non-cluster in input objects");
            std::abort();
        }
        fOriginalClusters.push_back(cluster);
        fTrackClusters.push_back(cluster);
    }

    int throttle = 5;
    std::size_t trackClusterOriginal;
    do {
        trackClusterOriginal = fTrackClusters.size();
        CaptNamedDebug("road", "Start another iteration");

        // Define a queue to hold the current key.
        SeedContainer currentSeed;
        
        // Find hits at the "upstream" end of the track.
        for (SeedContainer::iterator c = fTrackClusters.begin();
             c != fTrackClusters.end(); ++c) {
            currentSeed.push_back(*c);
            double length = (currentSeed.front()->GetPosition().Vect()
                             -currentSeed.back()->GetPosition().Vect()).Mag();
            CaptNamedDebug("road", "Add upstream to seed" << 
                           currentSeed.back()->GetPosition().Vect()
                           << "   size: " << currentSeed.size() 
                           << "   length: " << length);
            if (currentSeed.size() < fSeedSize) continue;
            if (length < fSeedLength) continue;
            break;
        }
        
        for (SeedContainer::iterator s = currentSeed.begin(); 
             s != currentSeed.end(); ++s) {
            SeedContainer::iterator where =
                std::find(fTrackClusters.begin(), fTrackClusters.end(), 
                          *s);
            if (where ==  fTrackClusters.end()) {
                CaptError("Seed not found in track.");
                continue;
            }
            CaptNamedDebug("road", "Remove upstream from track" << 
                           (*where)->GetPosition().Vect());
            fTrackClusters.erase(where);
        }
        
        CaptNamedDebug("road", "Follow road upstream");
        int collectedClusters = 0;
        while (!fRemainingClusters.empty() && currentSeed.size()>2) {
            CP::THandle<CP::TReconCluster> cluster
                = NextCluster(currentSeed, fRemainingClusters, false);
            
            // If a cluster wasn't found, then stop looking for more.
            if (!cluster) break;
            
            // Find the cluster being added to the seed and remove it from the
            // remaining clusters.  This keeps the cluster from being found
            // twice.
            RemainingContainer::iterator where =
                std::find(fRemainingClusters.begin(), fRemainingClusters.end(), 
                          cluster);
            if (where ==  fRemainingClusters.end()) {
                CaptError("Upstream cluster not found in remaining clusters");
            }
            CaptNamedDebug("road", "Remove from remaining " << 
                           (*where)->GetPosition().Vect());
            fRemainingClusters.erase(where);
            
            // Add the cluster to the seed.  We are searching to the upstream
            // end, so the cluster gets inserted at the beginning.
            CaptNamedDebug("road", "Add to seed upstream " << 
                           cluster->GetPosition().Vect());
            currentSeed.push_front(cluster);
            
            while (currentSeed.size() > fSeedSize
                   && ((currentSeed.front()->GetPosition().Vect()
                        -currentSeed.back()->GetPosition().Vect()).Mag()
                       >= fSeedLength)) {
                CaptNamedDebug("road", "Add to track upstream (pop seed)" << 
                               currentSeed.back()->GetPosition().Vect());
                fTrackClusters.push_front(currentSeed.back());
                currentSeed.pop_back();
            }
            if ((++collectedClusters)>fMaxClusters) break;
        }
        
        // At the end, flush the clusters in the current seed into the track. 
        for (SeedContainer::reverse_iterator c = currentSeed.rbegin();
             c != currentSeed.rend(); ++c) {
            CaptNamedDebug("road", "Add to track upstream (empty seed)" << 
                           (*c)->GetPosition().Vect());
            fTrackClusters.push_front(*c);
        }
        currentSeed.clear();
        
        // Find clusters at the downstream end of the track.
        for (SeedContainer::reverse_iterator c = fTrackClusters.rbegin();
             c != fTrackClusters.rend();
             ++c) {
            currentSeed.push_front(*c);
            double length = (currentSeed.front()->GetPosition().Vect()
                             -currentSeed.back()->GetPosition().Vect()).Mag();
            CaptNamedDebug("road", "Add downstream " << 
                           currentSeed.front()->GetPosition().Vect()
                           << "   size: " << currentSeed.size() 
                           << "   length: " << length);
            if (currentSeed.size() < fSeedSize) continue;
            if (length < fSeedLength) continue;
            break;
        }
        
        for (SeedContainer::iterator s = currentSeed.begin(); 
             s != currentSeed.end(); ++s) {
            SeedContainer::iterator where =
                std::find(fTrackClusters.begin(), fTrackClusters.end(), 
                          *s);
            if (where ==  fTrackClusters.end()) {
                CaptError("Seed not found in track.");
                continue;
            }
            fTrackClusters.erase(where);
        }
        
        collectedClusters = 0;
        CaptNamedDebug("road", "Follow road downstream");
        while (!fRemainingClusters.empty() && currentSeed.size()>2) {
            CP::THandle<CP::TReconCluster> cluster
                = NextCluster(currentSeed, fRemainingClusters, true);
            
            // If a cluster wasn't found, then stop looking for more.
            if (!cluster) break;
            
            // Find the cluster being added to the seed and remove it from the
            // remaining clusters.  This keeps the cluster from being found
            // twice.
            RemainingContainer::iterator where 
                = std::find(fRemainingClusters.begin(),
                            fRemainingClusters.end(), 
                            cluster);
            if (where == fRemainingClusters.end()) {
                CaptError("Downstream cluster not found in remaining clusters");
            }
            fRemainingClusters.erase(where);
            
            // Add the cluster to the seed.  We are searching to the upstream
            // end, so the cluster gets inserted at the beginning.
            currentSeed.push_back(cluster);
            
            while (currentSeed.size() > fSeedSize
                   && ((currentSeed.front()->GetPosition().Vect()
                        -currentSeed.back()->GetPosition().Vect()).Mag()
                       >= fSeedLength)) {
                fTrackClusters.push_back(currentSeed.front());
                currentSeed.pop_front();
            }
            if ((++collectedClusters)>fMaxClusters) break;
        }
        
        // At the end, flush the clusters in the current seed into the track. 
        for (SeedContainer::iterator c = currentSeed.begin();
             c != currentSeed.end(); ++c) {
            CaptNamedDebug("road", "Add to track downstream (empty seed)" << 
                           (*c)->GetPosition().Vect());
            fTrackClusters.push_back(*c);
        }
        currentSeed.clear();

        CaptLog("Road following started with "
                << trackClusterOriginal << " clusters"
                << " and ended with " << fTrackClusters.size()
                << " clusters (" << throttle << ")");
             
    } while (trackClusterOriginal != fTrackClusters.size()
             && --throttle > 0);

}

double 
CP::TLinearRoad::FindPositionPrincipal(TPrincipal& pca,
                                       const TVector3& position) {
    double X[3] = {position.X(), position.Y(), position.Z()};
    double P[3] = {0,0,0};
    pca.X2P(X,P);
    return P[0];
}

TVector3 
CP::TLinearRoad::FindPrincipalPosition(TPrincipal& pca,
                                       double principal) {
    double X[3] = {0,0,0};
    double P[3] = {principal,0,0};
    pca.P2X(P,X,3);
    return TVector3(X[0],X[1],X[2]);
}

CP::THandle<CP::TReconCluster>
CP::TLinearRoad::NextCluster(const SeedContainer& seed,
                             const RemainingContainer& clusters, 
                             bool extendBack) {
    
    double bestDistance = 100*unit::meter;
    CP::THandle<CP::TReconCluster> bestCluster;

    // The direction is estimated using a PCA analysis of the seed cluster
    // positions.
    std::unique_ptr<TPrincipal> principal(new TPrincipal(3,""));
    for (SeedContainer::const_iterator s = seed.begin();s != seed.end(); ++s) {
        double row1[3] = {(*s)->GetPosition().X()-(*s)->GetLongAxis().X(),
                          (*s)->GetPosition().Y()-(*s)->GetLongAxis().Y(),
                          (*s)->GetPosition().Z()-(*s)->GetLongAxis().Z()};
        double row2[3] = {(*s)->GetPosition().X(),
                         (*s)->GetPosition().Y(),
                         (*s)->GetPosition().Z()};
        double row3[3] = {(*s)->GetPosition().X()+(*s)->GetLongAxis().X(),
                          (*s)->GetPosition().Y()+(*s)->GetLongAxis().Y(),
                          (*s)->GetPosition().Z()+(*s)->GetLongAxis().Z()};
        for (double q = (*s)->GetEDeposit(); q > 0; q -= 1000) {
            principal->AddRow(row1);
            principal->AddRow(row2);
            principal->AddRow(row2);
            principal->AddRow(row2);
            principal->AddRow(row3);
        }
    }
    principal->MakePrincipals();

    // Find the extent of the seed (along the principal axis).  The extent is
    // calculated based on the min and max hit positions along the major axis.
    // The extent is then used to find the seedPosition.
    std::pair<double,double> extent(0,0);
    for (SeedContainer::const_iterator s = seed.begin();s != seed.end(); ++s) {
        for (CP::THitSelection::const_iterator h = (*s)->GetHits()->begin();
             h != (*s)->GetHits()->end(); ++h) {
            double p = FindPositionPrincipal(*principal,
                                             (*h)->GetPosition());
            if (extent.first > p) extent.first = p;
            if (extent.second < p) extent.second = p;
        }
    }

    // Find the position along the principal axis for the front and back of
    // the seed and use that to determine the end of the seed to be extended.
    double pFront = FindPositionPrincipal(*principal,
                                          seed.front()->GetPosition().Vect());
    double pBack = FindPositionPrincipal(*principal,
                                         seed.back()->GetPosition().Vect());
    double pPosition;

    if (extendBack) {
        if (pFront < pBack) pPosition = extent.second;
        else pPosition = extent.first;
    }
    else {
        if (pFront < pBack) pPosition = extent.first;
        else pPosition = extent.second;
    }
    TVector3 seedPosition = FindPrincipalPosition(*principal,pPosition);
    TVector3 seedDirection = FindPrincipalPosition(*principal,0.0);
    seedDirection = (seedPosition - seedDirection).Unit();

    // Check all of the clusters to see which one gets added next.
    for (RemainingContainer::const_iterator c = clusters.begin();
         c != clusters.end(); ++c) {
        CP::THandle<CP::TReconCluster> cluster = (*c);
        TVector3 diff = cluster->GetPosition().Vect() - seedPosition;
        
        // Only look in the mostly forward direction.  This allows a backward
        // look equal to the road width.
        double dotProd = diff*seedDirection;
        if (dotProd < -0.5*fRoadWidth) {
            continue;
        }

        // The find the road width at the position of the current cluster.
        // The road gets wider as the clusters get further away.
        double localWidth = fRoadWidth + dotProd*fOpeningAngle;

        // Find the transverse distance from the cluster to the road center.
        double transDist = (diff - dotProd*seedDirection).Mag();

        // Make sure the hit is inside the road.
        if (transDist > 0.5*localWidth) continue;

        // And keep the cluster closest to the current end point.
        if (dotProd < bestDistance) {
            // Make sure the gap between the seed and cluster isn't too big.
            // This looks for the closest cluster hit to the seed position.
            double clusterDist = 1*unit::kilometer;
            for (CP::THitSelection::iterator h = cluster->GetHits()->begin();
                 h != cluster->GetHits()->end(); ++h) {
                double dist = ((*h)->GetPosition() - seedPosition).Mag();
                clusterDist = std::min(dist,clusterDist);
            }
            if (clusterDist < fRoadStep) {
                bestCluster = cluster;
                bestDistance = dotProd;
            }
        }
    }
    
    if (bestCluster) {
        CaptNamedDebug("road", "Next cluster at " << bestDistance
                       << " at " << bestCluster->GetPosition().Vect());
    }
    return bestCluster;
}

// This method creates a TTrackState from a position and direction (with
// covariance matrices) and a THit.
CP::THandle<CP::TTrackState> 
CP::TLinearRoad::CreateTrackState(CP::THandle<CP::TReconCluster> object,
                                  const TVector3& direction) {
    CP::THandle<CP::TTrackState> tstate(new CP::TTrackState);
    
    // Set the EDeposit
    tstate->SetEDeposit(object->GetEDeposit());
    tstate->SetEDepositVariance(object->GetEDepositVariance());
    
    // Set the value and covariance matrix for the position
    tstate->SetPosition(object->GetPosition());      
    tstate->SetPositionVariance(object->GetPositionVariance().X(),
                                object->GetPositionVariance().Y(),
                                object->GetPositionVariance().Z(),
                                object->GetPositionVariance().T());
    
    tstate->SetDirection(direction);
    tstate->SetDirectionVariance(0,0,0);
    
    return tstate;
}

CP::THandle<CP::TReconTrack> CP::TLinearRoad::GetTrack()  {
    if (fTrackClusters.size() < 2) {
        CaptNamedDebug("road","No track found");
        return CP::THandle<CP::TReconTrack>();
    }

    CheckUnique(fTrackClusters.begin(), fTrackClusters.end());

    CP::THandle<CP::TReconTrack> track 
        = CP::CreateTrackFromClusters("TLinearRoad",
                                      fTrackClusters.begin(), 
                                      fTrackClusters.end());

    return track;     
}

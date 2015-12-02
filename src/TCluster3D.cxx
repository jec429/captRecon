#include "TCluster3D.hxx"
#include "TDriftPosition.hxx"
#include "CreateCluster.hxx"
#include "TShareCharge.hxx"
#include "TRemoveOutliers.hxx"

#include <THandle.hxx>
#include <TReconHit.hxx>
#include <TReconCluster.hxx>
#include <TCaptLog.hxx>
#include <CaptGeomId.hxx>
#include <HEPUnits.hxx>
#include <TUnitsTable.hxx> 
#include <TRuntimeParameters.hxx>

#include <algorithm>
#include <memory>
#include <cmath>

namespace {
    struct compareHitTime {
        bool operator () (const CP::THandle<CP::THit>& lhs,
                     const CP::THandle<CP::THit>& rhs) {
            return lhs->GetTime() < rhs->GetTime();
        }
    };
};

TVector3 CP::TCluster3D::PositionXY(const CP::THandle<CP::THit>& hit1,
                                    const CP::THandle<CP::THit>& hit2) {
    double x1 = hit1->GetPosition().X();
    double y1 = hit1->GetPosition().Y();
    double dx1 = hit1->GetYAxis().X();
    double dy1 = hit1->GetYAxis().Y();
    
    double x2 = hit2->GetPosition().X();
    double y2 = hit2->GetPosition().Y();
    double dx2 = hit2->GetYAxis().X();
    double dy2 = hit2->GetYAxis().Y();

    // Solve 
    //      x1 + s1*dx1 = x2 + s2*dx2;
    //      y1 + s1*dy1 = y2 + s2*dy2; 
    // for s1, s2
    
    // Only the first shift is used, but here is the full solution (for s1 and
    // s2):
    double s1 = -(dx2*(y1-y2)+dy2*x2-dy2*x1)/(dx2*dy1-dx1*dy2);
    // double s2 = -(dx1*(y1-y2)+dy1*x2-dy1*x1)/(dx2*dy1-dx1*dy2); 
    
    return TVector3( (x1+s1*dx1), (y1+s1*dy1), 0.0);
}

double CP::TCluster3D::TimeZero(const CP::THitSelection& pmts,
                                const CP::THitSelection& wires) {
    ///////////////////////////////////////////////////////////////////////
    // Find the event time zero.
    ///////////////////////////////////////////////////////////////////////

    if (pmts.empty()) return 0.0;
    
    std::vector<double> times;
    for (CP::THitSelection::const_iterator p = pmts.begin();
         p != pmts.end(); ++p) {
        times.push_back((*p)->GetTime());
    }
    std::sort(times.begin(),times.end());

    double t0 = 0.0;
    int maxHits = 0;
    for (std::vector<double>::iterator t = times.begin();
         t != times.end(); ++t) {
        std::vector<double>::iterator h = t;
        while (++h != times.end()) {
            if (*h - *t > 2*unit::microsecond) break;
        }
        int dh = h - t;
        if (dh > maxHits) {
            maxHits = dh;
            t0 = *t;
        }
    }

    return t0;
}

CP::TCluster3D::TCluster3D()
    : TAlgorithm("TCluster3D", "Cluster Wire Hits") {
    fMaxDrift
        = CP::TRuntimeParameters::Get().GetParameterD(
            "captRecon.cluster3d.maxDrift");

    // These need to be turned into parameters.
    fXSeparation = CP::TRuntimeParameters::Get().GetParameterD(
            "captRecon.cluster3d.xSeparation");
    fVSeparation = CP::TRuntimeParameters::Get().GetParameterD(
            "captRecon.cluster3d.vSeparation");
    fUSeparation = CP::TRuntimeParameters::Get().GetParameterD(
            "captRecon.cluster3d.uSeparation");

    // This is the determined by the minimum tick of the digitizer.  
    fMinSeparation = 500*unit::ns;

    fEnergyPerCharge = CP::TRuntimeParameters::Get().GetParameterD(
        "captRecon.energyPerCharge");

}

CP::TCluster3D::~TCluster3D() { }

double CP::TCluster3D::OverlapTime(double r1, double r2, double step) const {
#ifdef QUADRATURE_OVERLAP
    return std::sqrt(r1*r1 + r2*r2 + step*step);
#else
    return std::max(r1,std::max(r2,step));
#endif
}

namespace {
    double hitRMS(CP::THitSelection::iterator begin, 
                  CP::THitSelection::iterator end) {
        double mean = 0;
        double mean2 = 0;
        double w = 0.0;
        while (begin != end) {
            double v = (*begin)->GetCharge();
            mean += v;
            mean2 += v*v;
            w += 1.0;
            ++begin;
        }
        mean /= w;
        mean2 /= w;
        return std::sqrt(mean2 - mean*mean);
    }

    double hitMean(CP::THitSelection::iterator begin, 
                   CP::THitSelection::iterator end) {
        double mean = 0;
        double w = 0.0;
        while (begin != end) {
            double v = (*begin)->GetCharge();
            mean += v;
            w += 1.0;
            ++begin;
        }
        mean /= w;
        return mean;
    }

    double hitTotal(CP::THitSelection::iterator begin, 
                   CP::THitSelection::iterator end) {
        double mean = 0;
        while (begin != end) {
            double v = (*begin)->GetCharge();
            mean += v;
            ++begin;
        }
        return mean;
    }
};

CP::THandle<CP::TAlgorithmResult>
CP::TCluster3D::Process(const CP::TAlgorithmResult& wires,
                        const CP::TAlgorithmResult& pmts,
                        const CP::TAlgorithmResult&) {
    CaptLog("TCluster3D Process " << GetEvent().GetContext());
    CP::THandle<CP::THitSelection> wireHits = wires.GetHits();
    if (!wireHits) {
        CaptError("No input hits");
        return CP::THandle<CP::TAlgorithmResult>();
    }

    CP::THandle<CP::THitSelection> pmtHits = pmts.GetHits();
    if (!pmtHits || pmtHits->size() < 1) {
        CaptError("No PMT hits provide so time zero cannot be found");
        return CP::THandle<CP::TAlgorithmResult>();
    }

    double t0 = TimeZero(*pmtHits,*wireHits);
        
    CP::THandle<CP::TAlgorithmResult> result = CreateResult();
    std::auto_ptr<CP::THitSelection> used(new CP::THitSelection("used"));
    std::auto_ptr<CP::THitSelection> unused(new CP::THitSelection("unused"));
    std::auto_ptr<CP::THitSelection> clustered(new CP::THitSelection(
                                                   "clustered"));

    /// Only save the used and unused hits if there are fewer than this many
    /// 2D hits.
    const std::size_t hitLimit = 3000;

    std::vector<float> allRMS;
    std::auto_ptr<CP::THitSelection> xHits(new CP::THitSelection);
    std::auto_ptr<CP::THitSelection> vHits(new CP::THitSelection);
    std::auto_ptr<CP::THitSelection> uHits(new CP::THitSelection);
    for (CP::THitSelection::iterator h2 = wireHits->begin(); 
         h2 != wireHits->end(); ++h2) {
        int plane = CP::GeomId::Captain::GetWirePlane((*h2)->GetGeomId());
        allRMS.push_back((*h2)->GetTimeRMS());
        if (plane == CP::GeomId::Captain::kXPlane) {
            xHits->push_back(*h2);
        }
        else if (plane == CP::GeomId::Captain::kVPlane) {
            vHits->push_back(*h2);
        }
        else if (plane == CP::GeomId::Captain::kUPlane) {
            uHits->push_back(*h2);
        }
        else {
            CaptError("Invalid wire plane");
        }
        if (wireHits->size() < hitLimit) unused->push_back(*h2);
    }
    // Set the maximum time difference between 2D clusters that might become a
    // 3D hit.  This is set to be large (i.e. clusters that are spacially
    // separated by more than 25 mm.
    double maxDeltaT = 16*unit::microsecond;

#ifdef LIMIT_SEARCH
    // The time range of the search needs to be limited when there are a lot
    // of hits.  The form below works well for large numbers of hits, but
    // needs to be tuned for smaller numbers.  It's removed from the
    // calculation for now (2014/2/16) since I'm working on small events.  The
    // problem is that the full, unoptimized, calculation is approximately
    // O(nHits^3), so for large events this is a very slow.
    double deltaRMS = 2.0;
    maxDeltaT = deltaRMS*allRMS[allRMS.size()-1];
#endif

    std::sort(xHits->begin(), xHits->end(), compareHitTime());
    std::sort(vHits->begin(), vHits->end(), compareHitTime());
    std::sort(uHits->begin(), uHits->end(), compareHitTime());

    CP::TCaptLog::IncreaseIndentation();
    CaptLog("X Hits: " << xHits->size()
            << " V Hits: " << vHits->size()
            << " U Hits: " << uHits->size()
            << "  max(RMS): " << unit::AsString(maxDeltaT,"time"));
    CP::TCaptLog::DecreaseIndentation();

    CP::TDriftPosition drift;

    // The time must be drift corrected!
    double deltaT;

    int trials = 0;
    CP::THitSelection::iterator vBegin = vHits->begin();
    CP::THitSelection::iterator uBegin = uHits->begin();
    CP::THitSelection writableHits;
    for (CP::THitSelection::iterator xh=xHits->begin();
         xh!=xHits->end(); ++xh) {
        ++trials;
        double xTime = drift.GetTime(**xh);
        double xRMS = (*xh)->GetTimeRMS();
        while (vBegin != vHits->end() 
               && drift.GetTime(**vBegin) - xTime < - maxDeltaT) {
            ++vBegin;
        }
        while (uBegin != uHits->end()
               && drift.GetTime(**uBegin) - xTime < - maxDeltaT) {
            ++uBegin;
        }

        for (CP::THitSelection::iterator vh=vBegin;
             vh!=vHits->end(); ++vh) {
            ++trials;
            double vTime = drift.GetTime(**vh);
            deltaT = vTime - xTime;
            if (deltaT > maxDeltaT) break;
            deltaT = std::abs(deltaT);

            double vRMS = (*vh)->GetTimeRMS();

            for (CP::THitSelection::iterator uh=uBegin;
                 uh!=uHits->end(); ++uh) {
                ++trials;
                double uTime = drift.GetTime(**uh);
                
                deltaT = uTime - xTime;
                if (deltaT > maxDeltaT) break;
                deltaT = std::abs(deltaT);
                
                double uRMS = (*uh)->GetTimeRMS();

                // Check that the X and U wires overlap in time.
                if (deltaT > OverlapTime(fXSeparation*xRMS,
                                         fUSeparation*uRMS,
                                         fMinSeparation)) continue;
                
                // Check that the X and V wires overlap in time.
                deltaT = std::abs(vTime - xTime);                
                if (deltaT > OverlapTime(fXSeparation*xRMS,
                                         fVSeparation*vRMS,
                                         fMinSeparation)) continue;

                // Check that the U and V wires overlap in time.
                deltaT = std::abs(vTime - uTime);                
                if (deltaT > OverlapTime(fUSeparation*uRMS,
                                         fVSeparation*vRMS,
                                         fMinSeparation)) continue;

                // Find the points at which the wires cross and check that the
                // wires all cross and one "point".  Two millimeters is a
                // "magic number chosen based on the geometry for a 3mm
                // separation between the wires.  It needs to change if the
                // wire spacing changes.
                TVector3 p1(PositionXY(*xh,*vh));
                TVector3 p2(PositionXY(*xh,*uh));
                double dist = (p2-p1).Mag();
                if (dist > 2*unit::mm) continue;

                CP::TWritableReconHit hit(*xh,*vh,*uh);
                TVector3 p3(PositionXY(*vh,*uh));

                if (wireHits->size() < hitLimit) {
                    // These three wire hits make a 3D point.  Get them into
                    // the correct hit selections.
                    used->AddHit(*xh);
                    unused->RemoveHit(*xh);
                    
                    used->AddHit(*vh);
                    unused->RemoveHit(*vh);
                    
                    used->AddHit(*uh);
                    unused->RemoveHit(*uh);
                }

#ifdef USE_BEST_TIME
                // Set the time.  It's the wire hit time after drifting to Z
                // equal to zero.  Some of the wires might have overlapping
                // tracks in this time bin, or the track might be at a bad
                // angle for one of the wires, so use the time of the hit with
                // the lowest uncertainty.  This isn't exactly right, but it's
                // a pretty good estimate.  This also takes the time
                // uncertainty from the best measured wire.  The same charge
                // distribution is being measured three times, so there are a
                // lot of correlations and combining the three hits doesn't
                // reduce the uncertainty by sqrt(3) (i.e. the measured time
                // widths are correlated).  In a nutshell, this assumes that
                // the lowest uncertainty comes from the hit with the best
                // measurement, and the least overlap with other tracks.
                double tHit = drift.GetTime(**xh);
                double tUnc = (*xh)->GetTimeUncertainty();
                if ((*vh)->GetTimeUncertainty() < tUnc) {
                    tHit = drift.GetTime(**vh);
                    tUnc = (*vh)->GetTimeUncertainty();
                }
                if ((*uh)->GetTimeUncertainty() < tUnc) {
                    tHit = drift.GetTime(**uh);
                    tUnc = (*uh)->GetTimeUncertainty();
                }
                hit.SetTime(tHit); // This will be overridden to be time zero.
                hit.SetTimeUncertainty(tUnc);
#else
                // Average the times.
                double tXHit = drift.GetTime(**xh);
                double tXUnc = (*xh)->GetTimeUncertainty();
                tXUnc = tXUnc*tXUnc;
                double tVHit = drift.GetTime(**vh);
                double tVUnc = (*vh)->GetTimeUncertainty();
                tVUnc = tVUnc*tVUnc;
                double tUHit = drift.GetTime(**uh);
                double tUUnc = (*uh)->GetTimeUncertainty();
                tUUnc = tUUnc*tUUnc;
                double tHit = tXHit/tXUnc + tVHit/tVUnc + tUHit/tUUnc;
                double tUnc = 1.0/tXUnc + 1.0/tVUnc + 1.0/tUUnc;
                tHit /= tUnc;
                hit.SetTime(tHit); // This will be overridden to be time zero.
                tUnc = std::sqrt(3.0/tUnc); // sqrt(3) for correlations.
                hit.SetTimeUncertainty(tUnc);
#endif

                // The time RMS is determined by the RMS of the narrowest hit
                // in time.  This will slightly overestimate the time RMS for
                // the hit, but is a fairly good approximation.  The RMS could
                // be calculated by combining the PDFs to directly calculate a
                // combined RMS, but since the three 2D hits are measureing
                // the same charge distribution, that ignores the correlations
                // between the 2D hits.  The "min" method assumes the hits are
                // all correlated.
                double tRMS = std::min((*xh)->GetTimeRMS(),
                                       std::min((*vh)->GetTimeRMS(),
                                                (*uh)->GetTimeRMS()));
                hit.SetTimeRMS(tRMS);

                // For now set the charge to X wire charge.  This doesn't work
                // for overlapping hits but gives a reasonable estimate of the
                // energy deposition otherwise.  The U and V wires don't
                // measure the total charge very well.  The charge for
                // "overlapping" hits will need to be calculated once all of
                // the TReconHits are constructed.
                CaptNamedVerbose("Hit",
                              "X: "
                              << unit::AsString((*xh)->GetCharge(), 
                                                (*xh)->GetChargeUncertainty(), 
                                                "pe")
                              << "   V: " 
                              << unit::AsString((*vh)->GetCharge(), 
                                                (*vh)->GetChargeUncertainty(), 
                                                "pe")
                              << "   U: " 
                              << unit::AsString((*uh)->GetCharge(), 
                                                (*uh)->GetChargeUncertainty(), 
                                                "pe"));
                
                double w = (*xh)->GetChargeUncertainty();
                double charge = (*xh)->GetCharge()/(w*w);
                double chargeUnc = 1.0/(w*w);

#define USE_V_CHARGE
#ifdef USE_V_CHARGE
                w = (*vh)->GetChargeUncertainty();
                charge += (*vh)->GetCharge()/(w*w);
                chargeUnc += 1.0/(w*w);
#endif

#define USE_U_CHARGE
#ifdef USE_U_CHARGE
                w = (*uh)->GetChargeUncertainty();
                charge += (*uh)->GetCharge()/(w*w);
                chargeUnc += 1.0/(w*w);
#endif

                charge /= chargeUnc;
                chargeUnc = std::sqrt(1.0/chargeUnc);

                hit.SetCharge(charge);
                hit.SetChargeUncertainty(chargeUnc);
                
                // Find the position for the 3D hit.  Take the average
                // position of the crossing points as the hit position.  It's
                // at Z=0 with a time offset relative to that position.  This
                // should probably be charge weighted, but this is a
                // conservative "average" point.  It's possible that it should
                // be charged weighted, but I suspect not.  That needs to be
                // answered based on fit residuals and pulls.
                TVector3 pos = p1 + p2 + p3;
                pos *= 1.0/3.0;
                pos.SetZ(0.0);
                hit.SetPosition(pos);

                // Find the xyRMS and xyUncertainty.  This is not being done
                // correctly, but this should be an acceptable approximation
                // for now.  The approximation is based on the idea that the X
                // rms of the three 2D hits is perpendicular to the wire, and
                // takes that as an estimate of the hit size.  The Z RMS is
                // calculated based on the time RMS of the three hits.
                double xyRMS = 0.0;
                xyRMS += (*xh)->GetRMS().X()*(*xh)->GetRMS().X();
                xyRMS += (*vh)->GetRMS().X()*(*vh)->GetRMS().X();
                xyRMS += (*uh)->GetRMS().X()*(*uh)->GetRMS().X();
                xyRMS /= 3.0;
                xyRMS += xyRMS + dist*dist;
                xyRMS = std::sqrt(xyRMS);

                hit.SetRMS(
                    TVector3(xyRMS,xyRMS,
                             drift.GetAverageDriftVelocity()*tRMS));
                
                // For the XY uncertainty, assume a uniform position
                // distribution.  For the Z uncertainty, just use the time
                // uncertainty.
                hit.SetUncertainty(
                    TVector3(2.0*xyRMS/std::sqrt(12.),2.0*xyRMS/std::sqrt(12.),
                             drift.GetAverageDriftVelocity()*tUnc));

                // Correct for the time zero.
                TLorentzVector driftedPosition = drift.GetPosition(hit,t0);
                hit.SetTime(t0);
                hit.SetPosition(driftedPosition.Vect());

                writableHits.push_back(CP::THandle<CP::TWritableReconHit>(
                                         new CP::TWritableReconHit(hit)));
            }
        }
    }

    CaptNamedLog("Cluster","Number of 3D Hits: " << writableHits.size());

#ifdef REMOVE_OUTLIERS
    CP::TRemoveOutliers outliers;
    outliers.Apply(writableHits);
#endif

#define SHARE_CLUSTERED_CHARGE
#ifdef SHARE_CLUSTERED_CHARGE
    // Share the charge among the 3D hits so that the total charge in the
    // event is not overcounted.
    CP::TShareCharge share;

    // Fill the charge sharing object.
    for (CP::THitSelection::iterator h = writableHits.begin();
         h != writableHits.end(); ++h) {
        CP::ShareCharge::TMeasurementGroup& group = share.AddGroup(*h);
        CP::THandle<CP::TWritableReconHit> groupHit = *h;
        for (int i=0; i<groupHit->GetConstituentCount(); ++i) {
            CP::THandle<CP::THit> hit = groupHit->GetConstituent(i); 
            group.AddMeasurement(hit, hit->GetCharge());
        }
    }

    share.Solve();

    // Loops over the measurement groups, and update the charges of the 3D
    // hits.  Since the 3D hit handles reference the hits in the writableHits
    // THitSelection, this also updates the hits that will be copied into the
    // output.
    for (CP::TShareCharge::Groups::const_iterator g = share.GetGroups().begin();
         g != share.GetGroups().end(); ++g) {
        CP::THandle<CP::TWritableReconHit> groupHit = g->GetObject();
        double totalCharge = 0.0;
        double totalSigma = 0.0;
        for(CP::ShareCharge::TLinks::const_iterator c = g->GetLinks().begin();
            c != g->GetLinks().end(); ++c) {
            CP::THandle<CP::THit> hit = (*c)->GetMeasurement()->GetObject();
            double charge = (*c)->GetCharge();
            // Notice that the sigma is not reduced by the weight.  This is an
            // attempt to capture some of the extra charge error introduced by
            // the charge sharing, but it's not formally correct.
            double sigma = hit->GetChargeUncertainty();
            totalCharge += charge/(sigma*sigma);
            totalSigma += 1.0/(sigma*sigma);
        }
        totalCharge /= totalSigma;
        totalSigma = std::sqrt(1.0/totalSigma);
        groupHit->SetCharge(totalCharge);
        groupHit->SetChargeUncertainty(totalSigma);
    }
#endif

    // Copy the selection of writable hits into a selection of recon hits.
    for (CP::THitSelection::iterator h = writableHits.begin();
         h != writableHits.end(); ++h) {
        CP::THandle<CP::TWritableReconHit> hit = *h;
        // Don't include hits that have had all their charge taken away by the
        // charge sharing.  The  10 pe cut corresponds to a hit energy of
        // about 340 eV.
        if (hit->GetCharge() < 10.0) continue;
        clustered->push_back(
            CP::THandle<CP::TReconHit>(new CP::TReconHit(*hit)));
    }

    CaptNamedInfo("Cluster","Mean X Hit Charge " 
            << unit::AsString(hitMean(xHits->begin(), xHits->end()),
                              hitRMS(xHits->begin(), xHits->end()),
                              "pe"));
    CaptNamedInfo("Cluster","Total X Hit Charge " 
            << unit::AsString(hitTotal(xHits->begin(), xHits->end()),
                              "pe"));
    CaptNamedInfo("Cluster","Mean V Hit Charge " 
            << unit::AsString(hitMean(vHits->begin(), vHits->end()),
                              hitRMS(vHits->begin(), vHits->end()),
                              "pe"));
    CaptNamedInfo("Cluster","Total V Hit Charge " 
            << unit::AsString(hitTotal(vHits->begin(), vHits->end()),
                              "pe"));
    CaptNamedInfo("Cluster","Mean U Hit Charge "
            << unit::AsString(hitMean(uHits->begin(), uHits->end()),
                              hitRMS(uHits->begin(), uHits->end()),
                              "pe"));
    CaptNamedInfo("Cluster","Total U Hit Charge " 
            << unit::AsString(hitTotal(uHits->begin(), uHits->end()),
                              "pe"));

    CP::TReconObjectContainer* final = new CP::TReconObjectContainer("final");
    CP::THandle<CP::TReconCluster> usedCluster 
        = CreateCluster("clustered", clustered->begin(), clustered->end());
    final->push_back(usedCluster);
    result->AddResultsContainer(final);
    
    CaptLog("Total hit charge: " 
            << unit::AsString(usedCluster->GetEDeposit(),"pe")
            << " is " 
            << unit::AsString(fEnergyPerCharge*usedCluster->GetEDeposit(),
                              "energy")
            << " from " << clustered->size() << " hits");

    if (unused->size() > 0) result->AddHits(unused.release());
    if (used->size() > 0) result->AddHits(used.release());
    result->AddHits(clustered.release());

    return result;
}

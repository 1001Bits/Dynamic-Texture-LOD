#include "Fo4MeshOwners.h"
#include <cstdlib>
#include <iostream>

namespace {
    unsigned checks{};
    void Check(bool value,const char* message) {++checks;if(!value){std::cerr<<message<<'\n';std::exit(1);}}
    using Owners=Fo4Mesh::Pilot::OwnerProvenance;
    std::uint64_t Copy(Owners& owners,std::uint64_t shape=100,std::uint64_t source=10,std::uint64_t dest=20) {
        const auto ticket=owners.Begin(shape,source,dest,1,true,true);
        Check(ticket!=0,"unique audited source starts copy proof");
        Check(owners.AddRef(shape,1,ticket),"one scoped native AddRef attributed");
        Check(owners.Complete(ticket,2,true,true,true),"post-copy alias publication verified");
        return ticket;
    }
}
int main() {
    Owners owners;
    Check(!owners.Begin(100,10,20,2,true,true),"refs2 alone does not establish owners");
    Check(!owners.Begin(100,10,10,1,true,true),"self-copy is not two owners");
    Check(!owners.Begin(100,10,20,1,false,true),"unaudited template rejected");
    Check(!owners.Begin(100,10,20,1,true,false),"non-Mesh destination rejected");
    auto certificate=Copy(owners);
    Check(!owners.Certifies(100,2,certificate),"base copy is insufficient before final segment metadata");
    Check(!owners.ValidateDestination(100,10,2,true),"source visit cannot substitute for destination validation");
    Check(owners.ValidateDestination(100,20,2,true),"borrowed final destination metadata completes certificate");
    Check(owners.Includes(100,10,2,certificate) && owners.Includes(100,20,2,certificate),"both proven aliases admitted");
    Check(!owners.Includes(100,30,2,certificate),"same shape with foreign geometry rejected");
    Check(!owners.Certifies(101,2,certificate) && !owners.Certifies(100,1,certificate),"identity and count must match certificate");
    Check(!owners.Certifies(100,2,certificate+1),"stale certificate rejected");
    Check(!owners.AddRef(100,2,0),"future unscoped owner invalidates before increment");
    Check(!owners.Certifies(100,2,certificate),"3-to2 count ABA cannot revive certificate");
    const auto replacement=Copy(owners);
    Check(replacement!=certificate,"fresh copy cannot reuse prior certificate epoch");
    Check(owners.ValidateDestination(100,20,2,true),"fresh copy validates separately");
    Check(owners.Invalidate(100),"nonfinal DecRef invalidates before native decrement");
    Check(!owners.Certifies(100,2,replacement),"2-to1-to2 count ABA remains invalid");
    Check(!owners.Invalidate(100),"duplicate invalidation produces no duplicate proof receipt");
    auto pending=owners.Begin(100,10,20,1,true,true);
    Check(pending && owners.CopyIncludes(10,pending) && owners.CopyIncludes(20,pending),"initial copy scope identifies borrowed aliases");
    Check(!owners.CopyIncludes(30,pending) && !owners.CopyIncludes(10,0),"foreign/reentrant scope cannot bypass mutation invalidation");
    Check(!owners.AddRef(100,1,0),"concurrent same-shape AddRef cannot impersonate copying thread");
    Check(!owners.Complete(pending,2,true,true,true),"invalidated in-flight copy cannot certify");
    pending=owners.Begin(100,10,20,1,true,true);
    Check(owners.AddRef(100,1,pending),"new first AddRef observed");
    Check(!owners.AddRef(100,2,pending),"extra scoped AddRef invalidates proof");
    Check(!owners.Complete(pending,2,true,true,true),"extra increment hidden by count change cannot certify");
    pending=owners.Begin(100,10,20,1,true,true);
    Check(!owners.Begin(101,30,40,1,true,true),"nested copy fails closed without replacing pending identity");
    Check(!owners.Complete(pending,2,true,true,true),"nested copy invalidation is persistent");
    pending=owners.Begin(100,10,20,1,true,true);
    Check(!owners.Complete(pending,2,true,true,true),"unobserved AddRef does not qualify even with aliases and refs2");
    pending=owners.Begin(100,10,20,1,true,true);owners.AddRef(100,1,pending);
    Check(!owners.Complete(pending,2,false,true,true),"mismatched native pointer after publication rejected");
    pending=owners.Begin(100,10,20,1,true,true);owners.AddRef(100,1,pending);
    Check(!owners.Complete(pending,2,true,false,true),"changed source metadata invalidates copy");
    certificate=Copy(owners);
    Check(!owners.ValidateDestination(100,20,2,false),"wrong final derived segments invalidates proof");
    Check(!owners.Certifies(100,2,certificate),"later scalar count cannot revive bad destination metadata");
    certificate=Copy(owners);owners.ValidateDestination(100,20,2,true);
    Check(!owners.Invalidate(999) && owners.Certifies(100,2,certificate),"unrelated release preserves certificate");
    Check(owners.Invalidate(100),"final zero-ref transition invalidates before native object reuse");
    auto recycled=Copy(owners);owners.ValidateDestination(100,20,2,true);
    Check(recycled!=certificate && !owners.Certifies(100,2,certificate),"reused shape and alias addresses cannot revive old epoch");
    std::cout<<checks<<" owner provenance checks passed\n";
}

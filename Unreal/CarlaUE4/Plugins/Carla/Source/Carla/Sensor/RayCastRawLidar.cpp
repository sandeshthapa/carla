// Copyright (c) 2017 Computer Vision Center (CVC) at the Universitat Autonoma
// de Barcelona (UAB).
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include <PxScene.h>
#include <cmath>
#include "Carla.h"
#include "Carla/Sensor/RayCastRawLidar.h"
#include "Carla/Actor/ActorBlueprintFunctionLibrary.h"

#include <compiler/disable-ue4-macros.h>
#include "carla/geom/Math.h"
#include <compiler/enable-ue4-macros.h>

#include "DrawDebugHelpers.h"
#include "Engine/CollisionProfile.h"
#include "Runtime/Engine/Classes/Kismet/KismetMathLibrary.h"
#include "Runtime/Core/Public/Async/ParallelFor.h"

FActorDefinition ARayCastRawLidar::GetSensorDefinition()
{
  return UActorBlueprintFunctionLibrary::MakeLidarDefinition(TEXT("ray_cast_raw"));
}

ARayCastRawLidar::ARayCastRawLidar(const FObjectInitializer& ObjectInitializer)
  : Super(ObjectInitializer)
{
  PrimaryActorTick.bCanEverTick = true;
}

void ARayCastRawLidar::Set(const FActorDescription &ActorDescription)
{
  Super::Set(ActorDescription);
  FLidarDescription LidarDescription;
  UActorBlueprintFunctionLibrary::SetLidar(ActorDescription, LidarDescription);
  Set(LidarDescription);
}

void ARayCastRawLidar::Set(const FLidarDescription &LidarDescription)
{
  Description = LidarDescription;
  LidarRawData = FLidarRawData(Description.Channels);
  CreateLasers();
}

void ARayCastRawLidar::CreateLasers()
{
  const auto NumberOfLasers = Description.Channels;
  check(NumberOfLasers > 0u);
  const float DeltaAngle = NumberOfLasers == 1u ? 0.f :
    (Description.UpperFovLimit - Description.LowerFovLimit) /
    static_cast<float>(NumberOfLasers - 1);
  LaserAngles.Empty(NumberOfLasers);
  for(auto i = 0u; i < NumberOfLasers; ++i)
  {
    const float VerticalAngle =
        Description.UpperFovLimit - static_cast<float>(i) * DeltaAngle;
    LaserAngles.Emplace(VerticalAngle);
  }
}

void ARayCastRawLidar::Tick(const float DeltaTime)
{
  Super::Tick(DeltaTime);

  SimulateLidar(DeltaTime);

  auto DataStream = GetDataStream(*this);
  DataStream.Send(*this, LidarRawData, DataStream.PopBufferFromPool());
}

void ARayCastRawLidar::SimulateLidar(const float DeltaTime)
{
  const uint32 ChannelCount = Description.Channels;
  const uint32 PointsToScanWithOneLaser =
    FMath::RoundHalfFromZero(
        Description.PointsPerSecond * DeltaTime / float(ChannelCount));

  if (PointsToScanWithOneLaser <= 0)
  {
    UE_LOG(
        LogCarla,
        Warning,
        TEXT("%s: no points requested this frame, try increasing the number of points per second."),
        *GetName());
    return;
  }

  check(ChannelCount == LaserAngles.Num());

  const float CurrentHorizontalAngle = carla::geom::Math::ToDegrees(
      LidarRawData.GetHorizontalAngle());
  const float AngleDistanceOfTick = Description.RotationFrequency * 360.0f * DeltaTime;
  const float AngleDistanceOfLaserMeasure = AngleDistanceOfTick / PointsToScanWithOneLaser;

  ResetRecordedHits(ChannelCount, PointsToScanWithOneLaser);

  GetWorld()->GetPhysicsScene()->GetPxScene()->lockRead();
  ParallelFor(ChannelCount, [&](int32 idxChannel) {

    FCriticalSection Mutex;
    ParallelFor(PointsToScanWithOneLaser, [&](int32 idxPtsOneLaser) {
      FHitResult HitResult;
      float VertAngle = LaserAngles[idxChannel];
      float HorizAngle = CurrentHorizontalAngle + AngleDistanceOfLaserMeasure * idxPtsOneLaser;

      bool PreprocessResult = PreprocessRay(VertAngle, HorizAngle);

      if (PreprocessResult && ShootLaser(VertAngle, HorizAngle, HitResult)) {
        Mutex.Lock();
        WritePointAsync(idxChannel, HitResult);
        Mutex.Unlock();
      }
    });
  });
  GetWorld()->GetPhysicsScene()->GetPxScene()->unlockRead();

  FTransform ActorTransf = GetTransform();
  ComputeAndSaveDetections(ActorTransf);

  const float HorizontalAngle = carla::geom::Math::ToRadians(
      std::fmod(CurrentHorizontalAngle + AngleDistanceOfTick, 360.0f));
  LidarRawData.SetHorizontalAngle(HorizontalAngle);
}

  void ARayCastRawLidar::ResetRecordedHits(uint32_t Channels, uint32_t MaxPointsPerChannel) {
    RecordedHits.resize(Channels);
    for (auto& aux : RecordedHits) {
      aux.clear();
      aux.reserve(MaxPointsPerChannel);
    }
  }

  void ARayCastRawLidar::WritePointAsync(uint32_t channel, FHitResult &detection) {
    DEBUG_ASSERT(GetChannelCount() > channel);
    RecordedHits[channel].emplace_back(detection);
  }

  void ARayCastRawLidar::ComputeAndSaveDetections(const FTransform& SensorTransform) {
    std::vector<u_int32_t> PointsPerChannel(Description.Channels);

    for (auto idxChannel = 0u; idxChannel < Description.Channels; ++idxChannel)
      PointsPerChannel[idxChannel] = RecordedHits[idxChannel].size();
    LidarRawData.ResetSerPoints(PointsPerChannel);

    for (auto idxChannel = 0u; idxChannel < Description.Channels; ++idxChannel) {
      for (auto& hit : RecordedHits[idxChannel]) {
        FRawDetection detection;
        ComputeRawDetection(hit, SensorTransform, detection);

        LidarRawData.WritePointSync(detection);
      }
    }
  }

void ARayCastRawLidar::ComputeRawDetection(const FHitResult& HitInfo, const FTransform& SensorTransf, FRawDetection& Detection) const
{
    const FVector HitPoint = HitInfo.ImpactPoint;
    Detection.point = SensorTransf.Inverse().TransformPosition(HitPoint);

    const FVector VecInc = - (HitPoint - SensorTransf.GetLocation()).GetSafeNormal();
    Detection.cos_inc_angle = FVector::DotProduct(VecInc, HitInfo.ImpactNormal);

    const FActorRegistry &Registry = GetEpisode().GetActorRegistry();

    AActor* actor = HitInfo.Actor.Get();
    Detection.object_idx = 0;
    Detection.object_tag = static_cast<uint32_t>(ECityObjectLabel::None);

    if (actor != nullptr) {

      FActorView view = Registry.Find(actor);

      if(view.IsValid()) {
        const FActorInfo* ActorInfo = view.GetActorInfo();
        Detection.object_idx = ActorInfo->Description.UId;

        if(ActorInfo != nullptr) {
          TSet<ECityObjectLabel> labels = ActorInfo->SemanticTags;
          if(labels.Num() == 1)
              Detection.object_tag = static_cast<uint32_t>(*labels.CreateConstIterator());
        }
        else {
          //UE_LOG(LogCarla, Warning, TEXT("Info not valid!!!!"));
        }
      }
      else {
        //UE_LOG(LogCarla, Warning, TEXT("View is not valid %p!!!!"), view.GetActor());

      }
    }
    else {
      //UE_LOG(LogCarla, Warning, TEXT("Actor not valid %p!!!!"), actor);
    }
}


bool ARayCastRawLidar::ShootLaser(const float VerticalAngle, const float HorizontalAngle, FHitResult& HitResult) const
{
  FCollisionQueryParams TraceParams = FCollisionQueryParams(FName(TEXT("Laser_Trace")), true, this);
  TraceParams.bTraceComplex = true;
  TraceParams.bReturnPhysicalMaterial = false;

  FHitResult HitInfo(ForceInit);

  FTransform ActorTransf = GetTransform();
  FVector LidarBodyLoc = ActorTransf.GetLocation();
  FRotator LidarBodyRot = ActorTransf.Rotator();
  FRotator LaserRot (VerticalAngle, HorizontalAngle, 0);  // float InPitch, float InYaw, float InRoll
  FRotator ResultRot = UKismetMathLibrary::ComposeRotators(
    LaserRot,
    LidarBodyRot
  );
  const auto Range = Description.Range;
  FVector EndTrace = Range * UKismetMathLibrary::GetForwardVector(ResultRot) + LidarBodyLoc;

  GetWorld()->LineTraceSingleByChannel(
    HitInfo,
    LidarBodyLoc,
    EndTrace,
    ECC_GameTraceChannel2,
    TraceParams,
    FCollisionResponseParams::DefaultResponseParam
  );

  if (HitInfo.bBlockingHit) {
    HitResult = HitInfo;
    return true;
  } else {
    return false;
  }
}

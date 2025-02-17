// Copyright 1998-2017 Epic Games, Inc. All Rights Reserved.

#include "Compilation/MovieSceneSegmentCompiler.h"

FMovieSceneSectionData::FMovieSceneSectionData(const TRange<float>& InBounds, FSectionEvaluationData InEvalData, int32 InPriority)
	: Bounds(InBounds)
	, EvalData(InEvalData)
	, Priority(InPriority)
{}

void FMovieSceneSegmentCompilerRules::ProcessSegments(TArray<FMovieSceneSegment>& Segments, const TArrayView<const FMovieSceneSectionData>& SourceData) const
{
	if (!Segments.Num())
	{
		return;
	}

	for (FMovieSceneSegment& Segment : Segments)
	{
		BlendSegment(Segment, SourceData);
	}

	int32 Index = 0;

	// First off, add an open segment before the first segment if necessary/possible
	if (!Segments[0].Range.GetLowerBound().IsOpen())
	{
		InsertSegment(Segments, 0, SourceData);
	}
	++Index;

	// Then add any segments inbetween valid segments
	for (; Index < Segments.Num(); ++Index)
	{
		if (InsertSegment(Segments, Index, SourceData))
		{
			// Increment the counter to account for the added segment
			++Index;
		}
	}

	if (!Segments.Last().Range.GetUpperBound().IsOpen())
	{
		InsertSegment(Segments, Segments.Num(), SourceData);
	}

	PostProcessSegments(Segments, SourceData);
}

bool FMovieSceneSegmentCompilerRules::InsertSegment(TArray<FMovieSceneSegment>& Segments, int32 Index, const TArrayView<const FMovieSceneSectionData>& SourceData) const
{
	const FMovieSceneSegment* PreviousSegment = Segments.IsValidIndex(Index-1) ? &Segments[Index-1] : nullptr;
	const FMovieSceneSegment* NextSegment = Segments.IsValidIndex(Index) ? &Segments[Index] : nullptr;

	TRange<float> EmptyRange(
		PreviousSegment 	? TRangeBound<float>::FlipInclusion(PreviousSegment->Range.GetUpperBound()) : TRangeBound<float>(),
		NextSegment 		? TRangeBound<float>::FlipInclusion(NextSegment->Range.GetLowerBound()) 	: TRangeBound<float>()
		);

	if (EmptyRange.IsEmpty())
	{
		return false;
	}

	TOptional<FMovieSceneSegment> NewSeg = InsertEmptySpace(EmptyRange, PreviousSegment, NextSegment);
	if (!NewSeg.IsSet())
	{
		return false;
	}
	else if (!ensureMsgf(EmptyRange.Contains(NewSeg.GetValue().Range), TEXT("Attempting to insert an range that overflows the empty space. Correcting....")))
	{
		NewSeg.GetValue().Range = TRange<float>::Intersection(NewSeg.GetValue().Range, EmptyRange);
	}

	BlendSegment(NewSeg.GetValue(), SourceData);
	Segments.Insert(MoveTemp(NewSeg.GetValue()), Index);

	return true;
}

TArray<FMovieSceneSegment> FMovieSceneSegmentCompiler::Compile(const TArrayView<const FMovieSceneSectionData>& Data, const FMovieSceneSegmentCompilerRules* Rules)
{
	OverlappingSections.Reset(16);
	OverlappingRefCounts.Reset(16);
	LowerBounds.Reset(Data.Num());
	UpperBounds.Reset(Data.Num());
	CompiledSegments.Reset();

	LowerReadIndex = 0;
	UpperReadIndex = 0;

	// Populate the lists of lower/upper bounds
	for (int32 Index = 0; Index < Data.Num(); ++Index)
	{
		const FMovieSceneSectionData& Section = Data[Index];
		if (!Section.Bounds.IsEmpty())
		{
			ensure(Section.EvalData.ImplIndex != -1);
			LowerBounds.Add(FBound(Section.EvalData, Section.Bounds.GetLowerBound()));
			UpperBounds.Add(FBound(Section.EvalData, Section.Bounds.GetUpperBound()));
		}
	}

	LowerBounds.Sort(
		[](const FBound& A, const FBound& B)
		{
			return FFloatRangeBound::MinLower(A.Bound, B.Bound) == A.Bound;
		}
	);

	UpperBounds.Sort(
		[](const FBound& A, const FBound& B)
		{
			return FFloatRangeBound::MinUpper(A.Bound, B.Bound) == A.Bound;
		}
	);
	
	while (LowerReadIndex < LowerBounds.Num())
	{
		CloseCompletedSegments();

		TRangeBound<float> OpeningBound = LowerBounds[LowerReadIndex].Bound;

		const bool bHadOverlappingSections = OverlappingSections.Num() != 0;

		// Add the currently overlapping sections for any sections starting at exactly this time
		do
		{
			// Reference count how many times this section is overlapping the current time. This is to support multiple references to the same section.
			const int32 OverlapIndex = OverlappingSections.IndexOfByKey(LowerBounds[LowerReadIndex].EvalData);
			if (OverlapIndex == INDEX_NONE)
			{
				OverlappingSections.Add(LowerBounds[LowerReadIndex].EvalData);
				OverlappingRefCounts.Add(1);
			}
			else
			{
				++OverlappingRefCounts[OverlapIndex];
			}
		}
		while(++LowerReadIndex < LowerBounds.Num() && LowerBounds[LowerReadIndex].Bound == OpeningBound);

		CompiledSegments.Emplace(
			TRange<float>(OpeningBound, TRangeBound<float>()),
			OverlappingSections
			);
	}

	CloseCompletedSegments();

	ensure(OverlappingSections.Num() == 0);

	if (Rules)
	{
		Rules->ProcessSegments(CompiledSegments, Data);
	}

	return MoveTemp(CompiledSegments);
}

void FMovieSceneSegmentCompiler::CloseCompletedSegments()
{
	if (!CompiledSegments.Num())
	{
		return;
	}

	while(UpperReadIndex < UpperBounds.Num())
	{
		FMovieSceneSegment& LastSegment = CompiledSegments.Last();

		// If there is a non-empty range between the next lower bound and upper bound, we can't close any more
		const bool bHasOpeningRange = LowerReadIndex < LowerBounds.Num() && !TRange<float>(LowerBounds[LowerReadIndex].Bound, UpperBounds[UpperReadIndex].Bound).IsEmpty();
		if (bHasOpeningRange)
		{
			if (OverlappingSections.Num())
			{
				TRangeBound<float> ClosingBound = TRangeBound<float>::FlipInclusion(LowerBounds[LowerReadIndex].Bound);
				TRange<float> NewRange(LastSegment.Range.GetLowerBound(), ClosingBound);
				if (!NewRange.IsEmpty())
				{
					// Just set the closing bound of the last segment and return
					LastSegment.Range = NewRange;
				}
				else
				{
					// If it's empty, there's not point adding a segment, just allow the next segment to include the current OverlappingSegments
					CompiledSegments.RemoveAt(CompiledSegments.Num() - 1, 1, false);
				}
			}
			return;
		}

		TRangeBound<float> ClosingBound = UpperBounds[UpperReadIndex].Bound;
		// Update the last segment's closing range
		LastSegment.Range = TRange<float>(
			LastSegment.Range.GetLowerBound(),
			ClosingBound);

		ensure(!LastSegment.Range.IsEmpty());

		// Remove all sections that finish on this time
		while (UpperReadIndex < UpperBounds.Num() && UpperBounds[UpperReadIndex].Bound == ClosingBound)
		{
			int32 OverlappingIndex = OverlappingSections.IndexOfByKey(UpperBounds[UpperReadIndex].EvalData);
			if (ensure(OverlappingIndex != INDEX_NONE))
			{
				if (--OverlappingRefCounts[OverlappingIndex] == 0)
				{
					OverlappingSections.RemoveAtSwap(OverlappingIndex, 1, false);
					OverlappingRefCounts.RemoveAtSwap(OverlappingIndex, 1, false);
				}
			}
			++UpperReadIndex;
		}

		// If there are any more sections still active, create a new segment for those
		if (OverlappingSections.Num())
		{
			CompiledSegments.Emplace(
				TRange<float>(TRangeBound<float>::FlipInclusion(ClosingBound), TRangeBound<float>()),
				OverlappingSections
				);
		}
	}
}


FMovieSceneTrackCompiler::FRows::FRows(const TArray<UMovieSceneSection*>& Sections, const FMovieSceneSegmentCompilerRules* CompileRules)
{
	for (int32 Index = 0; Index < Sections.Num(); ++Index)
	{
		const UMovieSceneSection* Section = Sections[Index];
		if (!ensure(Section) || !Section->IsActive())
		{
			continue;
		}
		
		const int32 RowIndex = Section->GetRowIndex();
		if (!Rows.IsValidIndex(RowIndex))
		{
			Rows.AddDefaulted(RowIndex - Rows.Num() + 1);
		}

		const TRange<float> Range = Section->IsInfinite() ? TRange<float>::All() : Section->GetRange();

		FSectionEvaluationData EvalData(Rows[RowIndex].Sections.Num());

		Rows[RowIndex].Sections.Add(
			FMovieSceneSectionRowData(Index, Range, EvalData, Section->GetOverlapPriority())
			);

		if (!Range.GetLowerBound().IsOpen() && Section->GetPreRollTime() > 0)
		{
			EvalData.Flags = ESectionEvaluationFlags::PreRoll;
			TRange<float> PreRollRange(Range.GetLowerBoundValue() - Section->GetPreRollTime(), TRangeBound<float>::FlipInclusion(Range.GetLowerBoundValue()));
			Rows[RowIndex].Sections.Add(
				FMovieSceneSectionRowData(Index, PreRollRange, EvalData, Section->GetOverlapPriority())
				);
		}
		if (!Range.GetUpperBound().IsOpen() && Section->GetPostRollTime() > 0)
		{
			EvalData.Flags = ESectionEvaluationFlags::PostRoll;
			TRange<float> PostRollRange(TRangeBound<float>::FlipInclusion(Range.GetUpperBoundValue()), Range.GetUpperBoundValue() + Section->GetPostRollTime());
			Rows[RowIndex].Sections.Add(
				FMovieSceneSectionRowData(Index, PostRollRange, EvalData, Section->GetOverlapPriority())
				);
		}
	}

	Rows.RemoveAll(
		[](const FRow& Row)
		{
			return Row.Sections.Num() == 0;
		});

	for (FRow& Row : Rows)
	{
		Row.CompileRules = CompileRules;
	}
}

FMovieSceneTrackEvaluationField FMovieSceneTrackCompiler::Compile(const TArrayView<FRow>& Rows, const FMovieSceneSegmentCompilerRules* Rules)
{
	FMovieSceneTrackEvaluationField Result;

	// Methodology:
	//   - We initially run one segment compilation per row, specifying each FRow::Sections for the source data.
	//     This allows us to do per-row blending (like handling overlapping sections)
	//   - After each row, we accumulate a single array of compiled segments to be considered for compilation at the track level
	//     This allows us to do blending on a row basis, without considering individual row blending rules
	// 

	TArray<int32> SourceTrackDataToActualIndex;
	TArray<FMovieSceneSectionData> TrackCompileData;

	// Compile each row
	for (int32 RowIndex = 0; RowIndex < Rows.Num(); ++RowIndex)
	{
		const FRow& Row = Rows[RowIndex];
		if (!Row.Sections.Num())
		{
			continue;
		}
		
		FMovieSceneSegmentCompiler Compiler;
		
		// Compile this row into segments
		TArray<FMovieSceneSegment> RowSegments = Compiler.Compile(
			TArray<FMovieSceneSectionData>(Row.Sections),
			Row.CompileRules);

		const int32 Priority = Rows.Num() - RowIndex;
		for (FMovieSceneSegment& Segment : RowSegments)
		{
			for (FSectionEvaluationData& EvalData : Segment.Impls)
			{
				// Add the real section index to a LUT that corresponds to the source track compile data index
				// The is the index that's actually used at runtime
				SourceTrackDataToActualIndex.Add(Row.Sections[EvalData.ImplIndex].ActualSectionIndex);

				// The track compilation data requires impl indices into the source data (TrackCompileData) for the compiler rules' consideration
				EvalData.ImplIndex = TrackCompileData.Num();
				TrackCompileData.Add(FMovieSceneSectionData(Segment.Range, EvalData, Priority));
			}
		}
	}

	// Boil down each row into a single, blended field
	FMovieSceneSegmentCompiler Compiler;
	TArray<FMovieSceneSegment> TrackSegments = Compiler.Compile(TrackCompileData, nullptr);

	// At this point, ImplIndex members correspond to the source data array (TrackCompileData)
	if (Rules)
	{
		Rules->ProcessSegments(TrackSegments, TrackCompileData);
	}

	// Compile each of the row segments into a single segment comprising all overlapping sections on the track, blended and ordered appropriately
	// There should be no empty space by this point
	for (FMovieSceneSegment& Segment : TrackSegments)
	{
		for (FSectionEvaluationData& SectionEvalData : Segment.Impls)
		{
			// Remap the index to the actual section index
			SectionEvalData.ImplIndex = SourceTrackDataToActualIndex[SectionEvalData.ImplIndex];
		}
		
		if (Segment.Impls.Num() != 0 || (Rules && Rules->AllowEmptySegments()))
		{
			// If this is the same as the previous segment, and it ajoins the previous segment's range, just increase the range of the previous segment
			if (Result.Segments.Num() != 0 && Result.Segments.Last().Range.Adjoins(Segment.Range) && Result.Segments.Last().Impls == Segment.Impls)
			{
				Result.Segments.Last().Range = FFloatRange::Hull(Result.Segments.Last().Range, Segment.Range);
			}
			else
			{
				Result.Segments.Add(MoveTemp(Segment));
			}
		}
	}

	return Result;
}


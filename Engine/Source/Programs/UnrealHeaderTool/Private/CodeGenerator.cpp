// Copyright 1998-2017 Epic Games, Inc. All Rights Reserved.

#include "UnrealHeaderTool.h"
#include "CoreMinimal.h"
#include "Misc/AssertionMacros.h"
#include "HAL/PlatformProcess.h"
#include "Templates/UnrealTemplate.h"
#include "Math/UnrealMathUtility.h"
#include "Containers/UnrealString.h"
#include "UObject/NameTypes.h"
#include "Logging/LogMacros.h"
#include "CoreGlobals.h"
#include "HAL/FileManager.h"
#include "Misc/Parse.h"
#include "Misc/CoreMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Delegates/Delegate.h"
#include "Misc/Guid.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FeedbackContext.h"
#include "UObject/ErrorException.h"
#include "UObject/Script.h"
#include "UObject/ObjectMacros.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/MetaData.h"
#include "UObject/Interface.h"
#include "UObject/UnrealType.h"
#include "UObject/TextProperty.h"
#include "Misc/PackageName.h"
#include "UnrealHeaderToolGlobals.h"

#include "ParserClass.h"
#include "Scope.h"
#include "HeaderProvider.h"
#include "GeneratedCodeVersion.h"
#include "SimplifiedParsingClassInfo.h"
#include "UnrealSourceFile.h"
#include "ParserHelper.h"
#include "Classes.h"
#include "NativeClassExporter.h"
#include "ProfilingDebugging/ScopedTimers.h"
#include "HeaderParser.h"
#include "IScriptGeneratorPluginInterface.h"
#include "Manifest.h"
#include "StringUtils.h"
#include "Features/IModularFeatures.h"
#include "UHTMakefile/UHTMakefile.h"
#include "Algo/Sort.h"
#include "Algo/Reverse.h"

#include "FileLineException.h"

/////////////////////////////////////////////////////
// Globals

FManifest GManifest;

double GMacroizeTime = 0.0;

static TArray<FString> ChangeMessages;
static bool bWriteContents = false;
static bool bVerifyContents = false;

static TSharedRef<FUnrealSourceFile> PerformInitialParseOnHeader(UPackage* InParent, const TCHAR* FileName, EObjectFlags Flags, const TCHAR* Buffer, FUHTMakefile& UHTMakefile);

FCompilerMetadataManager GScriptHelper;

/** C++ name lookup helper */
FNameLookupCPP NameLookupCPP;

namespace
{
	static FString AsTEXT(FString InStr)
	{
		return FString::Printf(TEXT("TEXT(\"%s\")"), *InStr);
	}

	const TCHAR HeaderCopyright[] =
		TEXT("// Copyright 1998-2017 Epic Games, Inc. All Rights Reserved.\r\n")
		TEXT("/*===========================================================================\r\n")
		TEXT("\tGenerated code exported from UnrealHeaderTool.\r\n")
		TEXT("\tDO NOT modify this manually! Edit the corresponding .h files instead!\r\n")
		TEXT("===========================================================================*/\r\n")
		LINE_TERMINATOR;

	const TCHAR RequiredCPPIncludes[] = TEXT("#include \"GeneratedCppIncludes.h\"") LINE_TERMINATOR;
}

/**
 * Finds exact match of Identifier in string. Returns nullptr if none is found.
 *
 * @param StringBegin Start of string to search.
 * @param StringEnd End of string to search.
 * @param Identifier Identifier to find.
 * @return Pointer to Identifier match within string. nullptr if none found.
 */
const TCHAR* FindIdentifierExactMatch(const TCHAR* StringBegin, const TCHAR* StringEnd, const FString& Identifier)
{
	int32 StringLen = StringEnd - StringBegin;

	// Check for exact match first.
	if (FCString::Strncmp(StringBegin, *Identifier, StringLen) == 0)
	{
		return StringBegin;
	}

	int32        FindLen        = Identifier.Len();
	const TCHAR* StringToSearch = StringBegin;

	for (;;)
	{
		const TCHAR* IdentifierStart = FCString::Strstr(StringToSearch, *Identifier);
		if (IdentifierStart == nullptr)
		{
			// Not found.
			return nullptr;
		}

		if (IdentifierStart > StringEnd || IdentifierStart + FindLen + 1 > StringEnd)
		{
			// Found match is out of string range.
			return nullptr;
		}

		if (IdentifierStart == StringBegin && !FChar::IsIdentifier(*(IdentifierStart + FindLen + 1)))
		{
			// Found match is at the beginning of string.
			return IdentifierStart;
		}

		if (IdentifierStart + FindLen == StringEnd && !FChar::IsIdentifier(*(IdentifierStart - 1)))
		{
			// Found match ends with end of string.
			return IdentifierStart;
		}

		if (!FChar::IsIdentifier(*(IdentifierStart + FindLen)) && !FChar::IsIdentifier(*(IdentifierStart - 1)))
		{
			// Found match is in the middle of string
			return IdentifierStart;
		}

		// Didn't find exact match, nor got to end of search string. Keep on searching.
		StringToSearch = IdentifierStart + FindLen;
	}

	// We should never get here.
	checkNoEntry();
	return nullptr;
}

/**
 * Finds exact match of Identifier in string. Returns nullptr if none is found.
 *
 * @param String String to search.
 * @param Identifier Identifier to find.
 * @return Index to Identifier match within String. INDEX_NONE if none found.
 */
int32 FindIdentifierExactMatch(const FString& String, const FString& Identifier)
{
	const TCHAR* IdentifierPtr = FindIdentifierExactMatch(*String, *String + String.Len(), Identifier);
	if (IdentifierPtr == nullptr)
	{
		return INDEX_NONE;
	}

	return IdentifierPtr - *String;
}

/**
* Checks if exact match of Identifier is in String.
*
* @param StringBegin Start of string to search.
* @param StringEnd End of string to search.
* @param Identifier Identifier to find.
* @return true if Identifier is within string, false otherwise.
*/
bool HasIdentifierExactMatch(const TCHAR* StringBegin, const TCHAR* StringEnd, const FString& Find)
{
	return FindIdentifierExactMatch(StringBegin, StringEnd, Find) != nullptr;
}

/**
* Checks if exact match of Identifier is in String.
*
* @param String String to search.
* @param Identifier Identifier to find.
* @return true if Identifier is within String, false otherwise.
*/
bool HasIdentifierExactMatch(const FString &String, const FString& Identifier)
{
	return FindIdentifierExactMatch(String, Identifier) != INDEX_NONE;
}

/////////////////////////////////////////////////////
// FFlagAudit

//@todo: UCREMOVAL this is all audit stuff

static struct FFlagAudit
{
	struct Pair
	{
		FString Name;
		uint64 Flags;
		Pair(const UObject* Source, const TCHAR* FlagType, uint64 InFlags)
		{
			Name = Source->GetFullName() + TEXT("[") + FlagType + TEXT("]");
			Flags = InFlags;
		}
	};

	TArray<Pair> Items;

	void Add(const UObject* Source, const TCHAR* FlagType, uint64 Flags)
	{
		new (Items) Pair(Source, FlagType, Flags);
	}

	void WriteResults()
	{
		bool bDoDiff = false;
		FString Filename;
		FString RefFilename = FPaths::GameSavedDir() / TEXT("ReferenceFlags.txt");
		if( !FParse::Param( FCommandLine::Get(), TEXT("WRITEFLAGS") ) )
		{
			return;
		}
		if( FParse::Param( FCommandLine::Get(), TEXT("WRITEREF") ) )
		{
			Filename = RefFilename;
		}
		else if( FParse::Param( FCommandLine::Get(), TEXT("VERIFYREF") ) )
		{
			Filename = FPaths::GameSavedDir() / TEXT("VerifyFlags.txt");
			bDoDiff = true;
		}

		struct FComparePairByName
		{
			FORCEINLINE bool operator()( const FFlagAudit::Pair& A, const FFlagAudit::Pair& B ) const { return A.Name < B.Name; }
		};
		Items.Sort( FComparePairByName() );

		int32 MaxLen = 0;
		for (int32 Index = 0; Index < Items.Num(); Index++)
		{
			MaxLen = FMath::Max<int32>(Items[Index].Name.Len(), MaxLen);
		}
		MaxLen += 4;
		FStringOutputDevice File;
		for (int32 Index = 0; Index < Items.Num(); Index++)
		{
			File.Logf(TEXT("%s%s0x%016llx\r\n"), *Items[Index].Name, FCString::Spc(MaxLen - Items[Index].Name.Len()), Items[Index].Flags);
		}
		FFileHelper::SaveStringToFile(File, *Filename);
		if (bDoDiff)
		{
			FString Verify = File;
			FString Ref;
			if (FFileHelper::LoadFileToString(Ref, *RefFilename))
			{
				FStringOutputDevice MisMatches;
				TArray<FString> VerifyLines;
				Verify.ParseIntoArray(VerifyLines, TEXT("\n"), true);
				TArray<FString> RefLines;
				Ref.ParseIntoArray(RefLines, TEXT("\n"), true);
				check(VerifyLines.Num() == RefLines.Num()); // we aren't doing a sophisticated diff
				for (int32 Index = 0; Index < RefLines.Num(); Index++)
				{
					if (RefLines[Index] != VerifyLines[Index])
					{
						MisMatches.Logf(TEXT("REF   : %s"), *RefLines[Index]);
						MisMatches.Logf(TEXT("VERIFY: %s"), *VerifyLines[Index]);
					}
				}
				FString DiffFilename = FPaths::GameSavedDir() / TEXT("FlagsDiff.txt");
				FFileHelper::SaveStringToFile(MisMatches, *DiffFilename);
			}
		}
	}
} TheFlagAudit;

void ConvertToBuildIncludePath(const UPackage* Package, FString& LocalPath)
{
	FPaths::MakePathRelativeTo(LocalPath, *GPackageToManifestModuleMap.FindChecked(Package)->IncludeBase);
}

/**
 *	Helper function for finding the location of a package
 *	This is required as source now lives in several possible directories
 *
 *	@param	InPackage		The name of the package of interest
 *	@param	OutLocation		The location of the given package, if found
 *  @param	OutHeaderLocation	The directory where generated headers should be placed
 *
 *	@return	bool			true if found, false if not
 */
bool FindPackageLocation(const TCHAR* InPackage, FString& OutLocation, FString& OutHeaderLocation)
{
	// Mapping of processed packages to their locations
	// An empty location string means it was processed but not found
	static TMap<FString, FManifestModule*> CheckedPackageList;

	FString CheckPackage(InPackage);

	FManifestModule* ModuleInfoPtr = CheckedPackageList.FindRef(CheckPackage);

	if (!ModuleInfoPtr)
	{
		FManifestModule* ModuleInfoPtr2 = GManifest.Modules.FindByPredicate([&](FManifestModule& Module) { return Module.Name == CheckPackage; });
		if (ModuleInfoPtr2 && IFileManager::Get().DirectoryExists(*ModuleInfoPtr2->BaseDirectory))
		{
			ModuleInfoPtr = ModuleInfoPtr2;
			CheckedPackageList.Add(CheckPackage, ModuleInfoPtr);
		}
	}

	if (!ModuleInfoPtr)
	{
		return false;
	}

	OutLocation       = ModuleInfoPtr->BaseDirectory;
	OutHeaderLocation = ModuleInfoPtr->GeneratedIncludeDirectory;
	return true;
}


FString Macroize(const TCHAR* MacroName, const TCHAR* StringToMacroize)
{
	FScopedDurationTimer Tracker(GMacroizeTime);

	FString Result = StringToMacroize;
	if (Result.Len())
	{
		Result.ReplaceInline(TEXT("\r\n"), TEXT("\n"), ESearchCase::CaseSensitive);
		Result.ReplaceInline(TEXT("\n"), TEXT(" \\\n"), ESearchCase::CaseSensitive);
		checkSlow(Result.EndsWith(TEXT(" \\\n"), ESearchCase::CaseSensitive));

		if (Result.Len() >= 3)
		{
			for (int32 Index = Result.Len() - 3; Index < Result.Len(); ++Index)
			{
				Result[Index] = TEXT('\n');
			}
		}
		else
		{
			Result = TEXT("\n\n\n");
		}
		Result.ReplaceInline(TEXT("\n"), TEXT("\r\n"), ESearchCase::CaseSensitive);
	}
	return FString::Printf(TEXT("#define %s%s\r\n"), MacroName, Result.Len() ? TEXT(" \\") : TEXT("")) + Result;
}

/** Generates a CRC tag string for the specified field */
static FString GetGeneratedCodeCRCTag(UField* Field)
{
	FString Tag;
	const uint32* FieldCrc = GGeneratedCodeCRCs.Find(Field);	
	if (FieldCrc)
	{
		Tag = FString::Printf(TEXT(" // %u"), *FieldCrc);
	}
	return Tag;
}

struct FParmsAndReturnProperties
{
	FParmsAndReturnProperties()
		: Return(NULL)
	{
	}

	#if PLATFORM_COMPILER_HAS_DEFAULTED_FUNCTIONS

		FParmsAndReturnProperties(FParmsAndReturnProperties&&) = default;
		FParmsAndReturnProperties(const FParmsAndReturnProperties&) = default;
		FParmsAndReturnProperties& operator=(FParmsAndReturnProperties&&) = default;
		FParmsAndReturnProperties& operator=(const FParmsAndReturnProperties&) = default;

	#else

		FParmsAndReturnProperties(      FParmsAndReturnProperties&& Other) : Parms(MoveTemp(Other.Parms)), Return(MoveTemp(Other.Return)) {}
		FParmsAndReturnProperties(const FParmsAndReturnProperties&  Other) : Parms(         Other.Parms ), Return(         Other.Return ) {}
		FParmsAndReturnProperties& operator=(      FParmsAndReturnProperties&& Other) { Parms = MoveTemp(Other.Parms); Return = MoveTemp(Other.Return); return *this; }
		FParmsAndReturnProperties& operator=(const FParmsAndReturnProperties&  Other) { Parms =          Other.Parms ; Return =          Other.Return ; return *this; }

	#endif

	bool HasParms() const
	{
		return Parms.Num() || Return;
	}

	TArray<UProperty*> Parms;
	UProperty*         Return;
};

/**
 * Get parameters and return type for a given function.
 *
 * @param  Function The function to get the parameters for.
 * @return An aggregate containing the parameters and return type of that function.
 */
FParmsAndReturnProperties GetFunctionParmsAndReturn(UFunction* Function)
{
	FParmsAndReturnProperties Result;
	for ( TFieldIterator<UProperty> It(Function); It; ++It)
	{
		UProperty* Field = *It;

		if ((It->PropertyFlags & (CPF_Parm | CPF_ReturnParm)) == CPF_Parm)
		{
			Result.Parms.Add(Field);
		}
		else if (It->PropertyFlags & CPF_ReturnParm)
		{
			Result.Return = Field;
		}
	}
	return Result;
}

/**
 * Determines whether the glue version of the specified native function
 * should be exported
 *
 * @param	Function	the function to check
 * @return	true if the glue version of the function should be exported.
 */
bool ShouldExportUFunction(UFunction* Function)
{
	// export any script stubs for native functions declared in interface classes
	bool bIsBlueprintNativeEvent = (Function->FunctionFlags & FUNC_BlueprintEvent) && (Function->FunctionFlags & FUNC_Native);
	if (Function->GetOwnerClass()->HasAnyClassFlags(CLASS_Interface) && !bIsBlueprintNativeEvent)
	{
		return true;
	}

	// always export if the function is static
	if (Function->FunctionFlags & FUNC_Static)
	{
		return true;
	}

	// don't export the function if this is not the original declaration and there is
	// at least one parent version of the function that is declared native
	for (UFunction* ParentFunction = Function->GetSuperFunction(); ParentFunction; ParentFunction = ParentFunction->GetSuperFunction())
	{
		if (ParentFunction->FunctionFlags & FUNC_Native)
		{
			return false;
		}
	}

	return true;
}

FString CreateLiteralString(const FString& Str)
{
	FString Result;
	// Have a reasonable guess at reserving the right size
	Result.Reserve(Str.Len() + Result.Len());
	Result += TEXT("TEXT(\"");

	// Have a reasonable guess at reserving the right size
	Result.Reserve(Str.Len() + Result.Len());

	bool bPreviousCharacterWasHex = false;

	const TCHAR* Ptr = *Str;
	while (TCHAR Ch = *Ptr++)
	{
		switch (Ch)
		{
			case TEXT('\r'): continue;
			case TEXT('\n'): Result += TEXT("\\n");  bPreviousCharacterWasHex = false; break;
			case TEXT('\\'): Result += TEXT("\\\\"); bPreviousCharacterWasHex = false; break;
			case TEXT('\"'): Result += TEXT("\\\""); bPreviousCharacterWasHex = false; break;
			default:
				if (Ch < 31 || Ch >= 128)
				{
					Result += FString::Printf(TEXT("\\x%04x"), Ch);
					bPreviousCharacterWasHex = true;
				}
				else
				{
					// We close and open the literal (with TEXT) here in order to ensure that successive hex characters aren't appended to the hex sequence, causing a different number
					if (bPreviousCharacterWasHex && FCharWide::IsHexDigit(Ch))
					{
						Result += "\")TEXT(\"";
					}
					bPreviousCharacterWasHex = false;
					Result += Ch;
				}
				break;
		}
	}

	Result += TEXT("\")");
	return Result;
}

static FString GetMetaDataCodeForObject(const UObject* Object, const TCHAR* SymbolName, const TCHAR* Spaces)
{
	TMap<FName, FString>* MetaData = UMetaData::GetMapForObject(Object);

	FUHTStringBuilder Result;
	if (MetaData && MetaData->Num())
	{
		typedef TKeyValuePair<FName, FString> KVPType;
		TArray<KVPType> KVPs;
		for (TPair<FName, FString>& KVP : *MetaData)
		{
			KVPs.Add(KVPType(KVP.Key, KVP.Value));
		}

		// We sort the metadata here so that we can get consistent output across multiple runs
		// even when metadata is added in a different order
		KVPs.Sort([](const KVPType& Lhs, const KVPType& Rhs) { return Lhs.Key < Rhs.Key; });

		for (const KVPType& KVP : KVPs)
		{
			Result.Logf(TEXT("%sMetaData->SetValue(%s, TEXT(\"%s\"), %s);\r\n"), Spaces, SymbolName, *KVP.Key.ToString(), *CreateLiteralString(KVP.Value));
		}
	}
	return Result;
}

void FNativeClassHeaderGenerator::ExportProperties(FOutputDevice& Out, UStruct* Struct, int32 TextIndent)
{
	UProperty*	Previous			= NULL;
	UProperty*	PreviousNonEditorOnly = NULL;
	UProperty*	LastInSuper			= NULL;
	UStruct*	InheritanceSuper	= Struct->GetInheritanceSuper();
	bool		bEmittedHasEditorOnlyMacro = false;

	// Find last property in the lowest base class that has any properties
	UStruct* CurrentSuper = InheritanceSuper;
	while (LastInSuper == NULL && CurrentSuper)
	{
		for( TFieldIterator<UProperty> It(CurrentSuper,EFieldIteratorFlags::ExcludeSuper); It; ++It )
		{
			UProperty* Current = *It;

			// Disregard properties with 0 size like functions.
			if( It.GetStruct() == CurrentSuper && Current->ElementSize )
			{
				LastInSuper = Current;
			}
		}
		// go up a layer in the hierarchy
		CurrentSuper = CurrentSuper->GetSuperStruct();
	}

	// Iterate over all properties in this struct.
	for( TFieldIterator<UProperty> It(Struct, EFieldIteratorFlags::ExcludeSuper); It; ++It )
	{
		UProperty* Current = *It;

		// Disregard properties with 0 size like functions.
		if (It.GetStruct() == Struct)
		{
			// If we are switching from editor to non-editor or vice versa and the state of the WITH_EDITORONLY_DATA macro emission doesn't match, generate the 
			// #if or #endif appropriately.
			bool RequiresHasEditorOnlyMacro = Current->IsEditorOnlyProperty();
			if( !bEmittedHasEditorOnlyMacro && RequiresHasEditorOnlyMacro )
			{
				// Indent code and export CPP text.
				Out.Logf( TEXT("#if WITH_EDITORONLY_DATA\r\n") );
				bEmittedHasEditorOnlyMacro = true;
			}
			else if( bEmittedHasEditorOnlyMacro && !RequiresHasEditorOnlyMacro )
			{
				Out.Logf( TEXT("#endif // WITH_EDITORONLY_DATA\r\n") );
				bEmittedHasEditorOnlyMacro = false;
			}

			// Export property specifiers
			// Indent code and export CPP text.
			{
				FUHTStringBuilder JustPropertyDecl;

				const FString* Dim = GArrayDimensions.Find(Current);
				Current->ExportCppDeclaration( JustPropertyDecl, EExportedDeclaration::Member, Dim ? **Dim : NULL);
				ApplyAlternatePropertyExportText(*It, JustPropertyDecl, EExportingState::TypeEraseDelegates);

				// Finish up line.
				Out.Logf(TEXT("%s%s;\r\n"), FCString::Tab(TextIndent + 1), *JustPropertyDecl);
			}

			LastInSuper	= NULL;
			Previous = Current;
			if (!Current->IsEditorOnlyProperty())
			{
				PreviousNonEditorOnly = Current;
			}
		}
	}

	// End of property list.  If we haven't generated the WITH_EDITORONLY_DATA #endif, do so now.
	if (bEmittedHasEditorOnlyMacro)
	{
		Out.Log(TEXT("#endif // WITH_EDITORONLY_DATA\r\n"));
	}
}

/**
 * Class that is representing a type singleton.
 */
struct FTypeSingleton
{
public:
	/** Constructor */
	FTypeSingleton(FString InName, UField* InType)
		: Name(MoveTemp(InName)), Type(InType) {}

	/**
	 * Gets this singleton's name.
	 */
	const FString& GetName() const
	{
		return Name;
	}

	/**
	 * Gets this singleton's extern declaration.
	 */
	const FString& GetExternDecl() const
	{
		if (ExternDecl.IsEmpty())
		{
			ExternDecl = GenerateExternDecl(Type, GetName());
		}

		return ExternDecl;
	}

private:
	/**
	 * Extern declaration generator.
	 */
	static FString GenerateExternDecl(UField* InType, const FString& InName)
	{
		const TCHAR* TypeStr = nullptr;

		if (InType->GetClass() == UClass::StaticClass())
		{
			TypeStr = TEXT("UClass");
		}
		else if (InType->GetClass() == UFunction::StaticClass() || InType->GetClass() == UDelegateFunction::StaticClass())
		{
			TypeStr = TEXT("UFunction");
		}
		else if (InType->GetClass() == UScriptStruct::StaticClass())
		{
			TypeStr = TEXT("UScriptStruct");
		}
		else if (InType->GetClass() == UEnum::StaticClass())
		{
			TypeStr = TEXT("UEnum");
		}
		else
		{
			FError::Throwf(TEXT("Unsupported item type to get extern for."));
		}

		return FString::Printf(
			TEXT("\t%s_API class %s* %s;\r\n"),
			*FPackageName::GetShortName(InType->GetOutermost()).ToUpper(),
			TypeStr,
			*InName
		);
	}

	/** Field that stores this singleton name. */
	FString Name;

	/** Cached field that stores this singleton extern declaration. */
	mutable FString ExternDecl;

	/** Type of the singleton */
	UField* Type;
};

/**
 * Class that represents type singleton cache.
 */
class FTypeSingletonCache
{
public:
	/**
	 * Gets type singleton from cache.
	 *
	 * @param Type Singleton type.
	 * @param bRequiresValidObject Does it require a valid object?
	 */
	static const FTypeSingleton& Get(UField* Type, bool bRequiresValidObject = true)
	{
		static TMap<FTypeSingletonCacheKey, FTypeSingleton> CacheData;

		FTypeSingletonCacheKey Key(Type, bRequiresValidObject);
		if (FTypeSingleton* SingletonPtr = CacheData.Find(Key))
		{
			return *SingletonPtr;
		}

		return CacheData.Add(Key,
			FTypeSingleton(GenerateSingletonName(Type, bRequiresValidObject), Type)
		);
	}

private:
	/**
	 * Private type that represents cache map key.
	 */
	struct FTypeSingletonCacheKey
	{
		/** FTypeSingleton type */
		UField* Type;

		/** If this type singleton requires valid object. */
		bool bRequiresValidObject;

		/* Constructor */
		FTypeSingletonCacheKey(UField* InType, bool bInRequiresValidObject)
			: Type(InType), bRequiresValidObject(bInRequiresValidObject)
		{}

		/**
		 * Equality operator.
		 *
		 * @param Other Other key.
		 *
		 * @returns True if this is equal to Other. False otherwise.
		 */
		bool operator==(const FTypeSingletonCacheKey& Other) const
		{
			return Type == Other.Type && bRequiresValidObject == Other.bRequiresValidObject;
		}

		/**
		 * Gets hash value for this object.
		 */
		friend uint32 GetTypeHash(const FTypeSingletonCacheKey& Object)
		{
			return HashCombine(
				GetTypeHash(Object.Type),
				GetTypeHash(Object.bRequiresValidObject)
			);
		}
	};

	/**
	 * Generates singleton name.
	 */
	static FString GenerateSingletonName(UField* Item, bool bRequiresValidObject)
	{
		check(Item);

		FString Suffix;
		if (UClass* ItemClass = Cast<UClass>(Item))
		{
			if (ItemClass->HasAllClassFlags(CLASS_Intrinsic))
			{
				return FString::Printf(TEXT("%s::StaticClass()"), NameLookupCPP.GetNameCPP(ItemClass));
			}

			if (!bRequiresValidObject)
			{
				Suffix = TEXT("_NoRegister");
			}
		}

		FString Result;
		for (UObject* Outer = Item; Outer; Outer = Outer->GetOuter())
		{
			if (!Result.IsEmpty())
			{
				Result = TEXT("_") + Result;
			}

			if (Cast<UClass>(Outer) || Cast<UScriptStruct>(Outer))
			{
				FString OuterName = NameLookupCPP.GetNameCPP(Cast<UStruct>(Outer));
				Result = OuterName + Result;

				// Structs can also have UPackage outer.
				if (Cast<UClass>(Outer) || Cast<UPackage>(Outer->GetOuter()))
				{
					break;
				}
			}
			else
			{
				Result = Outer->GetName() + Result;
			}
		}

		// Can't use long package names in function names.
		if (Result.StartsWith(TEXT("/Script/"), ESearchCase::CaseSensitive))
		{
			Result = FPackageName::GetShortName(Result);
		}

		FString ClassString = NameLookupCPP.GetNameCPP(Item->GetClass());
		return FString(TEXT("Z_Construct_")) + ClassString + TEXT("_") + Result + Suffix + TEXT("()");
	}
};

FString FNativeClassHeaderGenerator::GetSingletonName(UField* Item, bool bRequiresValidObject)
{
	FString Result = FTypeSingletonCache::Get(Item, bRequiresValidObject).GetName();

	UClass* ItemClass = Cast<UClass>(Item);
	if (ItemClass != nullptr && ItemClass->HasAllClassFlags(CLASS_Intrinsic))
	{
		return Result;
	}

	if (CastChecked<UPackage>(Item->GetOutermost()) != Package)
	{
		// this is a cross module reference, we need to include the right extern decl
		FString Extern = FTypeSingletonCache::Get(Item, bRequiresValidObject).GetExternDecl();

		UniqueCrossModuleReferences.Add(*Extern);
	}
	return Result;
}

FString FNativeClassHeaderGenerator::GetOverriddenName(const UField* Item)
{
	FString OverriddenName = Item->GetMetaData(TEXT("OverrideNativeName"));
	if (!OverriddenName.IsEmpty())
	{
		return OverriddenName.ReplaceCharWithEscapedChar();
	}
	return Item->GetName();
}

FName FNativeClassHeaderGenerator::GetOverriddenFName(const UField* Item)
{
	FString OverriddenName = Item->GetMetaData(TEXT("OverrideNativeName"));
	if (!OverriddenName.IsEmpty())
	{
		return FName(*OverriddenName);
	}
	return Item->GetFName();
}

FString FNativeClassHeaderGenerator::GetOverriddenPathName(const UField* Item)
{
	return FString::Printf(TEXT("%s.%s"), *FClass::GetTypePackageName(Item), *GetOverriddenName(Item));
}

FString FNativeClassHeaderGenerator::GetOverriddenNameForLiteral(const UField* Item)
{
	FString OverriddenName = Item->GetMetaData(TEXT("OverrideNativeName"));
	if (!OverriddenName.IsEmpty())
	{
		return TEXT("TEXT(\"") + OverriddenName + TEXT("\")");
	}
	return TEXT("\"") + Item->GetName() + TEXT("\"");
}

FString FNativeClassHeaderGenerator::PropertyNew(FString& Meta, UProperty* Prop, const FString& OuterString, const FString& PropMacro, const TCHAR* Name, const TCHAR* Spaces, const TCHAR* SourceStruct)
{
	FString ExtraArgs;
	FString GeneratedCrc;

	FString PropNameDep = Prop->GetName();
	if (Prop->HasAllPropertyFlags(CPF_Deprecated))
	{
		PropNameDep += TEXT("_DEPRECATED");
	}

	if (UObjectPropertyBase* ObjectProperty = Cast<UObjectPropertyBase>(Prop))
	{
		UClass *TargetClass = ObjectProperty->PropertyClass;
		if (UClassProperty* ClassProperty = Cast<UClassProperty>(Prop))
		{
			TargetClass = ClassProperty->MetaClass;
		}
		if (UAssetClassProperty* SubclassOfProperty = Cast<UAssetClassProperty>(Prop))
		{
			TargetClass = SubclassOfProperty->MetaClass;
		}
		ExtraArgs = FString::Printf(TEXT(", %s"), *GetSingletonName(TargetClass, false)); 
		if (UClassProperty* ClassProperty = Cast<UClassProperty>(Prop))
		{
			ExtraArgs += FString::Printf(TEXT(", %s"), *GetSingletonName(ClassProperty->PropertyClass, false));
		}
	}
	else if (UInterfaceProperty* InterfaceProperty = Cast<UInterfaceProperty>(Prop))
	{
		UClass *TargetClass = InterfaceProperty->InterfaceClass;
		ExtraArgs = FString::Printf(TEXT(", %s"), *GetSingletonName(TargetClass, false)); 
	}
	else if (UStructProperty* StructProperty = Cast<UStructProperty>(Prop))
	{
		UScriptStruct* Struct = StructProperty->Struct;
		check(Struct);
		ExtraArgs = FString::Printf(TEXT(", %s"), *GetSingletonName(Struct));
	}
	else if (UByteProperty* ByteProperty = Cast<UByteProperty>(Prop))
	{
		if (ByteProperty->Enum)
		{
			ExtraArgs = FString::Printf(TEXT(", %s"), *GetSingletonName(ByteProperty->Enum));
		}
	}
	else if (UEnumProperty* EnumProperty = Cast<UEnumProperty>(Prop))
	{
		ExtraArgs = FString::Printf(TEXT(", %s"), *GetSingletonName(EnumProperty->Enum));
	}
	else if (UBoolProperty* BoolProperty = Cast<UBoolProperty>(Prop))
	{
		if (Cast<UArrayProperty>(BoolProperty->GetOuter()) || Cast<UMapProperty>(BoolProperty->GetOuter()) || Cast<USetProperty>(BoolProperty->GetOuter()))
		{
			ExtraArgs = FString(TEXT(", 0"));  // this is an array of C++ bools so the mask is irrelevant.
		}
		else
		{
			check(SourceStruct);
			ExtraArgs = FString::Printf(TEXT(", CPP_BOOL_PROPERTY_BITMASK(%s, %s)"), *PropNameDep, SourceStruct);
		}
		ExtraArgs += FString::Printf(TEXT(", sizeof(%s), %s"), *BoolProperty->GetCPPType(NULL, 0), BoolProperty->IsNativeBool() ? TEXT("true") : TEXT("false"));
	}
	else if (UDelegateProperty* DelegateProperty = Cast<UDelegateProperty>(Prop))
	{
		UFunction *TargetFunction = DelegateProperty->SignatureFunction;
		ExtraArgs = FString::Printf(TEXT(", %s"), *GetSingletonName(TargetFunction));
	}
	else if (UMulticastDelegateProperty* MulticastDelegateProperty = Cast<UMulticastDelegateProperty>(Prop))
	{
		UFunction *TargetFunction = MulticastDelegateProperty->SignatureFunction;
		ExtraArgs = FString::Printf(TEXT(", %s"), *GetSingletonName(TargetFunction));
	}

	auto GetPropName = [](UProperty* InProp) -> FString
	{
		if (!GUnsizedProperties.Contains(InProp))
		{
			return InProp->GetClass()->GetName();
		}

		if (InProp->IsA<UIntProperty>())
		{
			return TEXT("UnsizedIntProperty");
		}

		check(InProp->IsA<UUInt32Property>());
		return TEXT("UnsizedUIntProperty");
	};

	const TCHAR* UPropertyObjectFlags = FClass::IsOwnedByDynamicType(Prop) ? TEXT("RF_Public|RF_Transient") : TEXT("RF_Public|RF_Transient|RF_MarkAsNative");
	FString Constructor = FString::Printf(TEXT("new(EC_InternalUseOnlyConstructor, %s, TEXT(\"%s\"), %s) U%s(%s, 0x%016llx%s);"),
		*OuterString,
		*FNativeClassHeaderGenerator::GetOverriddenName(Prop),
		UPropertyObjectFlags,
		*GetPropName(Prop),
		*PropMacro,
		Prop->PropertyFlags & ~CPF_ComputedFlags, 
		*ExtraArgs);
	TheFlagAudit.Add(Prop, TEXT("PropertyFlags"), Prop->PropertyFlags);

	FString Lines  = FString::Printf(TEXT("%sUProperty* %s = %s%s\r\n"), 
		Spaces, 
		Name, 
		*Constructor,
		*GetGeneratedCodeCRCTag(Prop));

	if (Prop->ArrayDim != 1)
	{
		Lines += FString::Printf(TEXT("%s%s->ArrayDim = CPP_ARRAY_DIM(%s, %s);\r\n"), Spaces, Name, *PropNameDep, SourceStruct);
	}

	if (Prop->RepNotifyFunc != NAME_None)
	{
		Lines += FString::Printf(TEXT("%s%s->RepNotifyFunc = FName(TEXT(\"%s\"));\r\n"), Spaces, Name, *Prop->RepNotifyFunc.ToString());
	}
	Meta += GetMetaDataCodeForObject(Prop, Name, Spaces);
	return Lines;
}

void FNativeClassHeaderGenerator::OutputProperties(FString& Meta, FOutputDevice& OutputDevice, const FString& OuterString, const TArray<UProperty*>& Properties, const TCHAR* Spaces)
{
	bool bEmittedHasEditorOnlyMacro = false;
	for (int32 Index = Properties.Num() - 1; Index >= 0; Index--)
	{
		bool RequiresHasEditorOnlyMacro = Properties[Index]->IsEditorOnlyProperty();
		if  (!bEmittedHasEditorOnlyMacro && RequiresHasEditorOnlyMacro)
		{
			// Indent code and export CPP text.
			OutputDevice.Logf( TEXT("#if WITH_EDITORONLY_DATA\r\n") );
			bEmittedHasEditorOnlyMacro = true;
		}
		else if (bEmittedHasEditorOnlyMacro && !RequiresHasEditorOnlyMacro)
		{
			OutputDevice.Logf( TEXT("#endif // WITH_EDITORONLY_DATA\r\n") );
			bEmittedHasEditorOnlyMacro = false;
		}
		OutputProperty(Meta, OutputDevice, OuterString, Properties[Index], Spaces);
	}
	if (bEmittedHasEditorOnlyMacro)
	{
		OutputDevice.Logf( TEXT("#endif // WITH_EDITORONLY_DATA\r\n") );
	}
}

inline FString GetEventStructParamsName(UObject* Outer, const TCHAR* FunctionName)
{
	FString OuterName;
	if (Outer->IsA<UClass>())
	{
		OuterName = ((UClass*)Outer)->GetName();
	}
	else if (Outer->IsA<UPackage>())
	{
		OuterName = ((UPackage*)Outer)->GetName();
		OuterName.ReplaceInline(TEXT("/"), TEXT("_"), ESearchCase::CaseSensitive);
	}
	else
	{
		FError::Throwf(TEXT("Unrecognized outer type"));
	}

	FString Result = FString::Printf(TEXT("%s_event%s_Parms"), *OuterName, FunctionName);
	if (Result.Len() && FChar::IsDigit(Result[0]))
	{
		Result.InsertAt(0, TCHAR('_'));
	}
	return Result;
}

void FNativeClassHeaderGenerator::OutputProperty(FString& Meta, FOutputDevice& OutputDevice, const FString& OuterString, UProperty* Prop, const TCHAR* Spaces)
{
	FString PropName = Prop->GetName();

	FString PropVariableName = FString::Printf(TEXT("NewProp_%s"), *PropName);

	{
		FString SourceStruct;
		UFunction* Function = Cast<UFunction>(Prop->GetOuter());
		if (Function)
		{
			while (Function->GetSuperFunction())
			{
				Function = Function->GetSuperFunction();
			}
			FString FunctionName = Function->GetName();
			if( Function->HasAnyFunctionFlags( FUNC_Delegate ) )
			{
				FunctionName = FunctionName.LeftChop( FString( HEADER_GENERATED_DELEGATE_SIGNATURE_SUFFIX ).Len() );
			}

			SourceStruct = GetEventStructParamsName(Function->GetOuter(), *FunctionName);
		}
		else
		{
			SourceStruct = NameLookupCPP.GetNameCPP(CastChecked<UStruct>(Prop->GetOuter()));
		}
		FString PropMacroOuterClass;
		FString PropNameDep = PropName;
		if (Prop->HasAllPropertyFlags(CPF_Deprecated))
		{
			 PropNameDep += TEXT("_DEPRECATED");
		}
		UBoolProperty* BoolProperty = Cast<UBoolProperty>(Prop);
		if (BoolProperty)
		{
			OutputDevice.Logf(TEXT("%sCPP_BOOL_PROPERTY_BITMASK_STRUCT(%s, %s, %s);\r\n"), 
				Spaces, 
				*PropNameDep, 
				*SourceStruct,
				*BoolProperty->GetCPPType(NULL, 0));
			PropMacroOuterClass = FString::Printf(TEXT("FObjectInitializer(), EC_CppProperty, CPP_BOOL_PROPERTY_OFFSET(%s, %s)"),
				*PropNameDep, *SourceStruct);
		}
		else
		{
			PropMacroOuterClass = FString::Printf(TEXT("CPP_PROPERTY_BASE(%s, %s)"), *PropNameDep, *SourceStruct);
		}
		OutputDevice.Log(*PropertyNew(Meta, Prop, OuterString, PropMacroOuterClass, *PropVariableName, Spaces, *SourceStruct));
	}

	// Map of enum class properties to their outer's variable name
	TMap<UNumericProperty*, FString> UnderlyingEnumSuffixes;

	if (UArrayProperty* ArrayProperty = Cast<UArrayProperty>(Prop))
	{
		FString InnerOuterString = FString::Printf(TEXT("NewProp_%s"), *PropName);
		FString PropMacroOuterArray = TEXT("FObjectInitializer(), EC_CppProperty, 0");
		FString InnerVariableName = FString::Printf(TEXT("NewProp_%s_Inner"), *ArrayProperty->Inner->GetName());
		OutputDevice.Log(*PropertyNew(Meta, ArrayProperty->Inner, InnerOuterString, PropMacroOuterArray, *InnerVariableName, Spaces));

		if (UEnumProperty* EnumArrayProperty = Cast<UEnumProperty>(ArrayProperty->Inner))
		{
			UnderlyingEnumSuffixes.Add(EnumArrayProperty->UnderlyingProp, MoveTemp(InnerVariableName));
		}
	}

	else if (UMapProperty* MapProperty = Cast<UMapProperty>(Prop))
	{
		FString InnerOuterString = FString::Printf(TEXT("NewProp_%s"), *PropName);
		FString PropMacroOuterMap = TEXT("FObjectInitializer(), EC_CppProperty, ");
		FString KeyVariableName = FString::Printf(TEXT("NewProp_%s_KeyProp"), *MapProperty->KeyProp->GetName());
		FString ValueVariableName = FString::Printf(TEXT("NewProp_%s_ValueProp"), *MapProperty->ValueProp->GetName());
		OutputDevice.Log(*PropertyNew(Meta, MapProperty->KeyProp,   InnerOuterString, PropMacroOuterMap + TEXT("0"), *KeyVariableName,   Spaces));
		OutputDevice.Log(*PropertyNew(Meta, MapProperty->ValueProp, InnerOuterString, PropMacroOuterMap + TEXT("1"), *ValueVariableName, Spaces));

		if (UEnumProperty* EnumKeyProperty = Cast<UEnumProperty>(MapProperty->KeyProp))
		{
			UnderlyingEnumSuffixes.Add(EnumKeyProperty->UnderlyingProp, MoveTemp(KeyVariableName));
		}

		if (UEnumProperty* EnumValueProperty = Cast<UEnumProperty>(MapProperty->ValueProp))
		{
			UnderlyingEnumSuffixes.Add(EnumValueProperty->UnderlyingProp, MoveTemp(ValueVariableName));
		}
	}

	else if (USetProperty* SetProperty = Cast<USetProperty>(Prop))
	{
		FString InnerOuterString = FString::Printf(TEXT("NewProp_%s"), *PropName);
		FString PropMacroOuterSet = TEXT("FObjectInitializer(), EC_CppProperty, 0");
		FString ElementVariableName = FString::Printf(TEXT("NewProp_%s_ElementProp"), *SetProperty->ElementProp->GetName());
		OutputDevice.Log(*PropertyNew(Meta, SetProperty->ElementProp, InnerOuterString, PropMacroOuterSet, *ElementVariableName, Spaces));

		if (UEnumProperty* EnumSetProperty = Cast<UEnumProperty>(SetProperty->ElementProp))
		{
			UnderlyingEnumSuffixes.Add(EnumSetProperty->UnderlyingProp, MoveTemp(ElementVariableName));
		}
	}

	else if (UEnumProperty* EnumProperty = Cast<UEnumProperty>(Prop))
	{
		UnderlyingEnumSuffixes.Add(EnumProperty->UnderlyingProp, PropVariableName);
	}

	FString PropMacroOuterEnum = TEXT("FObjectInitializer(), EC_CppProperty, 0");
	for (const TPair<UNumericProperty*, FString>& PropAndVarName : UnderlyingEnumSuffixes)
	{
		OutputDevice.Log(*PropertyNew(Meta, PropAndVarName.Key, PropAndVarName.Value, PropMacroOuterEnum, *(PropAndVarName.Value + TEXT("_Underlying")), Spaces));
	}
}

static bool IsAlwaysAccessible(UScriptStruct* Script)
{
	FName ToTest = Script->GetFName();
	if (ToTest == NAME_Matrix)
	{
		return false; // special case, the C++ FMatrix does not have the same members.
	}
	bool Result = Script->HasDefaults(); // if we have cpp struct ops in it for UHT, then we can assume it is always accessible
	if( ToTest == NAME_Plane
		||	ToTest == NAME_Vector
		||	ToTest == NAME_Vector4 
		||	ToTest == NAME_Quat
		||	ToTest == NAME_Color
		)
	{
		check(Result);
	}
	return Result;
}

static void FindNoExportStructsRecursive(TArray<UScriptStruct*>& Structs, UStruct* Start)
{
	while (Start)
	{
		if (UScriptStruct* StartScript = Cast<UScriptStruct>(Start))
		{
			if (StartScript->StructFlags & STRUCT_Native)
			{
				break;
			}

			if (!IsAlwaysAccessible(StartScript)) // these are a special cases that already exists and if wrong if exported naively
			{
				// this will topologically sort them in reverse order
				Structs.Remove(StartScript);
				Structs.Add(StartScript);
			}
		}

		for (UProperty* Prop : TFieldRange<UProperty>(Start, EFieldIteratorFlags::ExcludeSuper))
		{
			if (UStructProperty* StructProp = Cast<UStructProperty>(Prop))
			{
				FindNoExportStructsRecursive(Structs, StructProp->Struct);
			}
			else if (UArrayProperty* ArrayProp = Cast<UArrayProperty>(Prop))
			{
				if (UStructProperty* InnerStructProp = Cast<UStructProperty>(ArrayProp->Inner))
				{
					FindNoExportStructsRecursive(Structs, InnerStructProp->Struct);
				}
			}
			else if (UMapProperty* MapProp = Cast<UMapProperty>(Prop))
			{
				if (UStructProperty* KeyStructProp = Cast<UStructProperty>(MapProp->KeyProp))
				{
					FindNoExportStructsRecursive(Structs, KeyStructProp->Struct);
				}
				if (UStructProperty* ValueStructProp = Cast<UStructProperty>(MapProp->ValueProp))
				{
					FindNoExportStructsRecursive(Structs, ValueStructProp->Struct);
				}
			}
			else if (USetProperty* SetProp = Cast<USetProperty>(Prop))
			{
				if (UStructProperty* ElementStructProp = Cast<UStructProperty>(SetProp->ElementProp))
				{
					FindNoExportStructsRecursive(Structs, ElementStructProp->Struct);
				}
			}
		}
		Start = Start->GetSuperStruct();
	}
}

static TArray<UScriptStruct*> FindNoExportStructs(UStruct* Start)
{
	TArray<UScriptStruct*> Result;
	FindNoExportStructsRecursive(Result, Start);

	// These come out in reverse order of topology so reverse them
	Algo::Reverse(Result);

	return Result;
}

FString GetPackageSingletonName(const UPackage* Package)
{
	static FString ClassString = NameLookupCPP.GetNameCPP(UPackage::StaticClass());
	return FString(TEXT("Z_Construct_")) + ClassString + TEXT("_") + Package->GetName().Replace(TEXT("/"), TEXT("_")) + TEXT("()");
}

void FNativeClassHeaderGenerator::ExportGeneratedPackageInitCode(FOutputDevice& Out, FUHTStringBuilder& OutDeclarations, const UPackage* InPackage, uint32 CRC)
{
	FString ApiString = GetAPIString();
	FString SingletonName = GetPackageSingletonName(InPackage);

	OutDeclarations.Logf(TEXT("\t%sclass UPackage* %s;\r\n"), *ApiString, *SingletonName);

	Out.Logf(TEXT("\tUPackage* %s\r\n"), *SingletonName);
	Out.Logf(TEXT("\t{\r\n"));
	Out.Logf(TEXT("\t\tstatic UPackage* ReturnPackage = nullptr;\r\n"));
	Out.Logf(TEXT("\t\tif (!ReturnPackage)\r\n"));
	Out.Logf(TEXT("\t\t{\r\n"));
	Out.Logf(TEXT("\t\t\tReturnPackage = CastChecked<UPackage>(StaticFindObjectFast(UPackage::StaticClass(), nullptr, FName(TEXT(\"%s\")), false, false));\r\n"), *InPackage->GetName());

	FString Meta = GetMetaDataCodeForObject(InPackage, TEXT("ReturnPackage"), TEXT("\t\t\t"));
	if (Meta.Len())
	{
		Out.Logf(TEXT("#if WITH_METADATA\r\n"));
		Out.Logf(TEXT("\t\t\tUMetaData* MetaData = ReturnPackage->GetMetaData();\r\n"));
		Out.Log(*Meta);
		Out.Logf(TEXT("#endif\r\n"));
	}

	Out.Logf(TEXT("\t\t\tReturnPackage->SetPackageFlags(PKG_CompiledIn | 0x%08X);\r\n"), InPackage->GetPackageFlags() & (PKG_ClientOptional | PKG_ServerSideOnly | PKG_EditorOnly | PKG_Developer));
	TheFlagAudit.Add(InPackage, TEXT("PackageFlags"), InPackage->GetPackageFlags());

	FGuid Guid;
	Guid.A = CRC;
	Guid.B = GenerateTextCRC(*OutDeclarations);
	Out.Logf(TEXT("\t\t\tFGuid Guid;\r\n"));
	Out.Logf(TEXT("\t\t\tGuid.A = 0x%08X;\r\n"), Guid.A);
	Out.Logf(TEXT("\t\t\tGuid.B = 0x%08X;\r\n"), Guid.B);
	Out.Logf(TEXT("\t\t\tGuid.C = 0x%08X;\r\n"), Guid.C);
	Out.Logf(TEXT("\t\t\tGuid.D = 0x%08X;\r\n"), Guid.D);
	Out.Logf(TEXT("\t\t\tReturnPackage->SetGuid(Guid);\r\n"), Guid.D);
	Out.Log(TEXT("\r\n"));

	for (UField* ScriptType : TObjectRange<UField>())
	{
		if (ScriptType->GetOutermost() != InPackage)
		{
			continue;
		}

		if (
			(ScriptType->IsA<UScriptStruct>() && (((UScriptStruct*)ScriptType)->StructFlags & STRUCT_NoExport) != 0) ||
			ScriptType->IsA<UDelegateFunction>()
			)
		{
			UField* FieldOuter = Cast<UField>(ScriptType->GetOuter());
			if (!FieldOuter || !FClass::IsDynamic(FieldOuter))
			{
				Out.Logf(TEXT("\t\t\t%s;\r\n"), *GetSingletonName(ScriptType));
			}
		}
	}

	Out.Logf(TEXT("\t\t}\r\n"));
	Out.Logf(TEXT("\t\treturn ReturnPackage;\r\n"));
	Out.Logf(TEXT("\t}\r\n"));
}

void FNativeClassHeaderGenerator::ExportNativeGeneratedInitCode(FOutputDevice& Out, FOutputDevice& OutDeclarations, const FUnrealSourceFile& SourceFile, FClass* Class, FUHTStringBuilder& OutFriendText)
{
	check(!OutFriendText.Len());

	const bool   bIsNoExport  = Class->HasAnyClassFlags(CLASS_NoExport);
	const bool   bIsDynamic   = FClass::IsDynamic(Class);
	const TCHAR* ClassNameCPP = NameLookupCPP.GetNameCPP(Class);

	FUHTStringBuilder BodyText;
	FUHTStringBuilder CallSingletons;
	FString ApiString = GetAPIString();

	TSet<FName> AlreadyIncludedNames;
	TArray<UFunction*> FunctionsToExport;
	for( TFieldIterator<UFunction> Function(Class,EFieldIteratorFlags::ExcludeSuper); Function; ++Function )
	{
		UFunction* LocalFunc = *Function;
		FName TrueName = FNativeClassHeaderGenerator::GetOverriddenFName(LocalFunc);
		bool bAlreadyIncluded = false;
		AlreadyIncludedNames.Add(TrueName, &bAlreadyIncluded);
		if (bAlreadyIncluded)
		{
			// In a dynamic class the same function signature may be used for a Multi- and a Single-cast delegate.
			if (!LocalFunc->IsA<UDelegateFunction>() || !bIsDynamic)
			{
				FError::Throwf(TEXT("The same function linked twice. Function: %s Class: %s"), *LocalFunc->GetName(), *Class->GetName());
			}
			continue;
		}
		FunctionsToExport.Add(*Function);
	}

	// Sort the list of functions
	FunctionsToExport.Sort();

	// Export the init code for each function
	for (UFunction* Function : FunctionsToExport)
	{
		if (!Function->IsA<UDelegateFunction>())
		{
			OutDeclarations.Log(FTypeSingletonCache::Get(Function).GetExternDecl());
			ExportFunction(Out, SourceFile, Function, bIsNoExport);
		}

		CallSingletons.Logf(TEXT("\t\t\t\tOuterClass->LinkChild(%s);\r\n"), *GetSingletonName(Function));
	}

	FUHTStringBuilder GeneratedClassRegisterFunctionText;

	// The class itself.
	{
		// simple ::StaticClass wrapper to avoid header, link and DLL hell
		{
			FString SingletonNameNoRegister(GetSingletonName(Class, false));

			OutDeclarations.Log(FTypeSingletonCache::Get(Class, false).GetExternDecl());

			GeneratedClassRegisterFunctionText.Logf(TEXT("\tUClass* %s\r\n"), *SingletonNameNoRegister);
			GeneratedClassRegisterFunctionText.Logf(TEXT("\t{\r\n"));
			GeneratedClassRegisterFunctionText.Logf(TEXT("\t\treturn %s::StaticClass();\r\n"), ClassNameCPP);
			GeneratedClassRegisterFunctionText.Logf(TEXT("\t}\r\n"));
		}
		FString SingletonName = GetSingletonName(Class);

		OutFriendText.Logf(TEXT("\tfriend %sclass UClass* %s;\r\n"), *ApiString, *SingletonName);
		OutDeclarations.Log(FTypeSingletonCache::Get(Class).GetExternDecl());

		GeneratedClassRegisterFunctionText.Logf(TEXT("\tUClass* %s\r\n"), *SingletonName);
		GeneratedClassRegisterFunctionText.Logf(TEXT("\t{\r\n"));
		if (!bIsDynamic)
		{
			GeneratedClassRegisterFunctionText.Logf(TEXT("\t\tstatic UClass* OuterClass = NULL;\r\n"));
			GeneratedClassRegisterFunctionText.Logf(TEXT("\t\tif (!OuterClass)\r\n"));
		}
		else
		{
			const FString DynamicClassPackageName = FClass::GetTypePackageName(Class);
			GeneratedClassRegisterFunctionText.Logf(TEXT("\t\tUPackage* OuterPackage = FindOrConstructDynamicTypePackage(TEXT(\"%s\"));\r\n"), *DynamicClassPackageName);
			GeneratedClassRegisterFunctionText.Logf(TEXT("\t\tUClass* OuterClass = Cast<UClass>(StaticFindObjectFast(UClass::StaticClass(), OuterPackage, TEXT(\"%s\")));\r\n"), *FNativeClassHeaderGenerator::GetOverriddenName(Class));
			GeneratedClassRegisterFunctionText.Logf(TEXT("\t\tif (!OuterClass || !(OuterClass->ClassFlags & CLASS_Constructed))\r\n"));
		}
		
		GeneratedClassRegisterFunctionText.Logf(TEXT("\t\t{\r\n"));
		if (Class->GetSuperClass() && Class->GetSuperClass() != Class)
		{
			GeneratedClassRegisterFunctionText.Logf(TEXT("\t\t\t%s;\r\n"), *GetSingletonName(Class->GetSuperClass()));
		}
		if (!bIsDynamic)
		{
			GeneratedClassRegisterFunctionText.Logf(TEXT("\t\t\t%s;\r\n"), *GetPackageSingletonName(CastChecked<UPackage>(Class->GetOutermost())));
		}
		GeneratedClassRegisterFunctionText.Logf(TEXT("\t\t\tOuterClass = %s::StaticClass();\r\n"), ClassNameCPP);
		GeneratedClassRegisterFunctionText.Logf(TEXT("\t\t\tif (!(OuterClass->ClassFlags & CLASS_Constructed))\r\n"), ClassNameCPP);
		GeneratedClassRegisterFunctionText.Logf(TEXT("\t\t\t{\r\n"), ClassNameCPP);
		GeneratedClassRegisterFunctionText.Logf(TEXT("\t\t\t\tUObjectForceRegistration(OuterClass);\r\n"));
		uint32 Flags = (Class->ClassFlags & CLASS_SaveInCompiledInClasses) | CLASS_Constructed;
		GeneratedClassRegisterFunctionText.Logf(TEXT("\t\t\t\tOuterClass->ClassFlags |= 0x%08X;\r\n"), Flags);
		TheFlagAudit.Add(Class, TEXT("ClassFlags"), Flags);
		GeneratedClassRegisterFunctionText.Logf(TEXT("\r\n"));
		GeneratedClassRegisterFunctionText.Log(CallSingletons);
		GeneratedClassRegisterFunctionText.Logf(TEXT("\r\n"));

		FString OuterString = TEXT("OuterClass");

		TMap<FName, FString>* MetaDataMap = UMetaData::GetMapForObject(Class);
		{
			FClassMetaData* ClassMetaData = GScriptHelper.FindClassData(Class);
			if (MetaDataMap && ClassMetaData && ClassMetaData->bObjectInitializerConstructorDeclared)
			{
				MetaDataMap->Add(FName(TEXT("ObjectInitializerConstructorDeclared")), FString());
			}
		}

		FString Meta = GetMetaDataCodeForObject(Class, *OuterString, TEXT("\t\t\t\t"));
		// properties
		{
			TArray<UProperty*> Props;
			for ( TFieldIterator<UProperty> ItInner(Class,EFieldIteratorFlags::ExcludeSuper); ItInner; ++ItInner )
			{
				Props.Add(*ItInner);
			}

			if (Props.Num() > 0)
			{
				GeneratedClassRegisterFunctionText.Logf(TEXT("PRAGMA_DISABLE_DEPRECATION_WARNINGS\r\n"));
				OutputProperties(Meta, GeneratedClassRegisterFunctionText, OuterString, Props, TEXT("\t\t\t\t"));
				GeneratedClassRegisterFunctionText.Logf(TEXT("PRAGMA_ENABLE_DEPRECATION_WARNINGS\r\n"));
			}
		}
		// function table
		{

			// Grab and sort the list of functions in the function map
			TArray<UFunction*> FunctionsInMap;
			for (auto Function : TFieldRange<UFunction>(Class, EFieldIteratorFlags::ExcludeSuper))
			{
				FunctionsInMap.Add(Function);
			}
			FunctionsInMap.Sort();

			// Emit code to construct each UFunction and rebuild the function map at runtime
			for (UFunction* Function : FunctionsInMap)
			{
				GeneratedClassRegisterFunctionText.Logf(TEXT("\t\t\t\tOuterClass->AddFunctionToFunctionMapWithOverriddenName(%s, %s);%s\r\n"), *GetSingletonName(Function), *FNativeClassHeaderGenerator::GetOverriddenNameForLiteral(Function), *GetGeneratedCodeCRCTag(Function));
			}
		}

		// class flags are handled by the intrinsic bootstrap code
		//GeneratedClassRegisterFunctionText.Logf(TEXT("\t\t\tOuterClass->ClassFlags = 0x%08X;\r\n"), Class->ClassFlags);
		if (Class->ClassConfigName != NAME_None)
		{
			GeneratedClassRegisterFunctionText.Logf(TEXT("\t\t\t\tOuterClass->ClassConfigName = FName(TEXT(\"%s\"));\r\n"), *Class->ClassConfigName.ToString());
		}

		GeneratedClassRegisterFunctionText.Logf(TEXT("\t\t\t\tstatic TCppClassTypeInfo<TCppClassTypeTraits<%s> > StaticCppClassTypeInfo;\r\n"), NameLookupCPP.GetNameCPP(Class, Class->HasAllClassFlags(CLASS_Interface)));
		GeneratedClassRegisterFunctionText.Logf(TEXT("\t\t\t\tOuterClass->SetCppTypeInfo(&StaticCppClassTypeInfo);\r\n"));

		for (auto& Inter : Class->Interfaces)
		{
			check(Inter.Class);
			FString OffsetString(TEXT("0"));
			if (Inter.PointerOffset)
			{
				OffsetString = FString::Printf(TEXT("VTABLE_OFFSET(%s, %s)"), ClassNameCPP, NameLookupCPP.GetNameCPP(Inter.Class, true));
			}
			GeneratedClassRegisterFunctionText.Logf(TEXT("\t\t\t\tOuterClass->Interfaces.Add(FImplementedInterface(%s, %s, %s ));\r\n"), 
				*GetSingletonName(Inter.Class, false),
				*OffsetString,
				Inter.bImplementedByK2 ? TEXT("true") : TEXT("false")
				);
		}
		if (Class->ClassGeneratedBy)
		{
			UE_LOG(LogCompile, Fatal, TEXT("For intrinsic and compiled-in classes, ClassGeneratedBy should always be NULL"));
			GeneratedClassRegisterFunctionText.Logf(TEXT("\t\t\t\tOuterClass->ClassGeneratedBy = %s;\r\n"), *GetSingletonName(CastChecked<UClass>(Class->ClassGeneratedBy), false));
		}

		GeneratedClassRegisterFunctionText.Logf(TEXT("\t\t\t\tOuterClass->StaticLink();\r\n"));

		if (Meta.Len())
		{
			GeneratedClassRegisterFunctionText.Logf(TEXT("#if WITH_METADATA\r\n"));
			GeneratedClassRegisterFunctionText.Logf(TEXT("\t\t\t\tUMetaData* MetaData = OuterClass->GetOutermost()->GetMetaData();\r\n"));
			GeneratedClassRegisterFunctionText.Log(*Meta);
			GeneratedClassRegisterFunctionText.Logf(TEXT("#endif\r\n"));
		}

		if (bIsDynamic)
		{
			FString* CustomDynamicClassInitializationMD = MetaDataMap ? MetaDataMap->Find(TEXT("CustomDynamicClassInitialization")) : nullptr;
			if (CustomDynamicClassInitializationMD)
			{
				GeneratedClassRegisterFunctionText.Logf(TEXT("\t\t\t\t%s(CastChecked<UDynamicClass>(OuterClass));\n"), *(*CustomDynamicClassInitializationMD));
			}
		}

		GeneratedClassRegisterFunctionText.Logf(TEXT("\t\t\t}\r\n"));
		
		GeneratedClassRegisterFunctionText.Logf(TEXT("\t\t}\r\n"));
		GeneratedClassRegisterFunctionText.Logf(TEXT("\t\tcheck(OuterClass->GetClass());\r\n"));
		GeneratedClassRegisterFunctionText.Logf(TEXT("\t\treturn OuterClass;\r\n"));
		GeneratedClassRegisterFunctionText.Logf(TEXT("\t}\r\n"));

		Out.Logf(TEXT("%s"), *GeneratedClassRegisterFunctionText);
	}

	if (OutFriendText.Len() && bIsNoExport)
	{
		Out.Logf(TEXT("\t/* friend declarations for pasting into noexport class %s\r\n"), ClassNameCPP);
		Out.Log(OutFriendText);
		Out.Logf(TEXT("\t*/\r\n"));
		OutFriendText.Reset();
	}

	FString SingletonName = GetSingletonName(Class);
	SingletonName.ReplaceInline(TEXT("()"), TEXT(""), ESearchCase::CaseSensitive); // function address

	FString OverriddenClassName = *FNativeClassHeaderGenerator::GetOverriddenName(Class);

	const FString InitSearchableValuesFunctionName = bIsDynamic ? Class->GetMetaData(TEXT("InitializeStaticSearchableValues")) : FString();
	const FString InitSearchableValuesFunctionParam = InitSearchableValuesFunctionName.IsEmpty() ? FString(TEXT("nullptr")) :
		FString::Printf(TEXT("&%s::%s"), ClassNameCPP, *InitSearchableValuesFunctionName);

	// Append base class' CRC at the end of the generated code, this will force update derived classes
	// when base class changes during hot-reload.
	uint32 BaseClassCRC = 0;
	if (Class->GetSuperClass() && !Class->GetSuperClass()->HasAnyClassFlags(CLASS_Intrinsic))
	{
		BaseClassCRC = GGeneratedCodeCRCs.FindChecked(Class->GetSuperClass());
	}
	GeneratedClassRegisterFunctionText.Logf(TEXT("\r\n// %u\r\n"), BaseClassCRC);

	// Calculate generated class initialization code CRC so that we know when it changes after hot-reload
	uint32 ClassCrc = GenerateTextCRC(*GeneratedClassRegisterFunctionText);
	GGeneratedCodeCRCs.Add(Class, ClassCrc);
	UHTMakefile.AddGeneratedCodeCRC(&SourceFile, Class, ClassCrc);
	// Emit the IMPLEMENT_CLASS macro to go in the generated cpp file.
	if (!bIsDynamic)
	{
		Out.Logf(TEXT("\tIMPLEMENT_CLASS(%s, %u);\r\n"), ClassNameCPP, ClassCrc);
	}
	else
	{
		Out.Logf(TEXT("\tIMPLEMENT_DYNAMIC_CLASS(%s, TEXT(\"%s\"), %u);\r\n"), ClassNameCPP, *OverriddenClassName, ClassCrc);
	}

	Out.Logf(TEXT("\tstatic FCompiledInDefer Z_CompiledInDefer_UClass_%s(%s, &%s::StaticClass, TEXT(\"%s\"), TEXT(\"%s\"), %s, %s, %s, %s);\r\n"),
		ClassNameCPP,
		*SingletonName,
		ClassNameCPP,
		bIsDynamic ? *FClass::GetTypePackageName(Class) : *Class->GetOutermost()->GetName(),
		bIsDynamic ? *OverriddenClassName : ClassNameCPP,
		bIsDynamic ? TEXT("true") : TEXT("false"),
		bIsDynamic ? *AsTEXT(FClass::GetTypePackageName(Class)) : TEXT("nullptr"),
		bIsDynamic ? *AsTEXT(FNativeClassHeaderGenerator::GetOverriddenPathName(Class)) : TEXT("nullptr"),
		*InitSearchableValuesFunctionParam);
}

void FNativeClassHeaderGenerator::ExportFunction(FOutputDevice& Out, const FUnrealSourceFile& SourceFile, UFunction* Function, bool bIsNoExport)
{
	UFunction* SuperFunction = Function->GetSuperFunction();

	bool bIsDelegate = Function->HasAnyFunctionFlags(FUNC_Delegate);

	const FString SingletonName = GetSingletonName(Function);

	FUHTStringBuilder CurrentFunctionText;

	CurrentFunctionText.Logf(TEXT("\tUFunction* %s\r\n"), *SingletonName);
	CurrentFunctionText.Logf(TEXT("\t{\r\n"));

	if (bIsNoExport || !(Function->FunctionFlags&FUNC_Event))  // non-events do not export a params struct, so lets do that locally for offset determination
	{
		TArray<UScriptStruct*> Structs = FindNoExportStructs(Function);
		for (UScriptStruct* Struct : Structs)
		{
			ExportMirrorsForNoexportStruct(CurrentFunctionText, Struct, /*Indent=*/ 2);
		}

		ExportEventParm(CurrentFunctionText, ForwardDeclarations, Function, /*Indent=*/ 2, /*bOutputConstructor=*/ false, EExportingState::TypeEraseDelegates);
	}

	if (UObject* Outer = Function->GetOuter())
	{
		CurrentFunctionText.Logf(TEXT("\t\tUObject* Outer=%s;\r\n"), Outer->IsA<UPackage>() ? *GetPackageSingletonName((UPackage*)Outer) : *GetSingletonName(Function->GetOwnerClass()));
	}
	else
	{
		CurrentFunctionText.Logf(TEXT("\t\tUObject* Outer=nullptr;\r\n"));
	}

	UField* FieldOuter = Cast<UField>(Function->GetOuter());
	const bool bIsDynamic = (FieldOuter && FClass::IsDynamic(FieldOuter));

	if (!bIsDynamic)
	{
		CurrentFunctionText.Logf(TEXT("\t\tstatic UFunction* ReturnFunction = NULL;\r\n"));
	}
	else
	{
		CurrentFunctionText.Logf(TEXT("\t\tUFunction* ReturnFunction = static_cast<UFunction*>(StaticFindObjectFast( UFunction::StaticClass(), Outer, %s ));\r\n")
			, *FNativeClassHeaderGenerator::GetOverriddenNameForLiteral(Function));
	}

	CurrentFunctionText.Logf(TEXT("\t\tif (!ReturnFunction)\r\n"));
	CurrentFunctionText.Logf(TEXT("\t\t{\r\n"));
	FString SuperFunctionString(TEXT("NULL"));
	if (SuperFunction)
	{
		SuperFunctionString = GetSingletonName(SuperFunction);
	}
	TArray<UProperty*> Props;
	for (TFieldIterator<UProperty> ItInner(Function, EFieldIteratorFlags::ExcludeSuper); ItInner; ++ItInner)
	{
		Props.Add(*ItInner);
	}
	FString StructureSize;
	if (Props.Num())
	{
		UFunction* TempFunction = Function;
		while (TempFunction->GetSuperFunction())
		{
			TempFunction = TempFunction->GetSuperFunction();
		}
		FString FunctionName = TempFunction->GetName();
		if (TempFunction->HasAnyFunctionFlags(FUNC_Delegate))
		{
			FunctionName = FunctionName.LeftChop(FString(HEADER_GENERATED_DELEGATE_SIGNATURE_SUFFIX).Len());
		}

		StructureSize = FString::Printf(TEXT(", sizeof(%s)"), *GetEventStructParamsName(TempFunction->GetOuter(), *FunctionName));
	}

	const TCHAR* UFunctionType = bIsDelegate ? TEXT("UDelegateFunction") : TEXT("UFunction");
	const TCHAR* UFunctionObjectFlags = FClass::IsOwnedByDynamicType(Function) ? TEXT("RF_Public|RF_Transient") : TEXT("RF_Public|RF_Transient|RF_MarkAsNative");
	CurrentFunctionText.Logf(TEXT("\t\t\tReturnFunction = new(EC_InternalUseOnlyConstructor, Outer, TEXT(\"%s\"), %s) %s(FObjectInitializer(), %s, 0x%08X, %d%s);\r\n"),
		*FNativeClassHeaderGenerator::GetOverriddenName(Function),
		UFunctionObjectFlags,
		UFunctionType,
		*SuperFunctionString,
		Function->FunctionFlags,
		(uint32)Function->RepOffset,
		*StructureSize
		);
	TheFlagAudit.Add(Function, TEXT("FunctionFlags"), Function->FunctionFlags);


	FString OuterString = FString(TEXT("ReturnFunction"));
	FString Meta = GetMetaDataCodeForObject(Function, *OuterString, TEXT("\t\t\t"));

	for (int32 Index = Props.Num() - 1; Index >= 0; Index--)
	{
		OutputProperty(Meta, CurrentFunctionText, OuterString, Props[Index], TEXT("\t\t\t"));
	}

	FFunctionData* CompilerInfo = FFunctionData::FindForFunction(Function);
	const FFuncInfo& FunctionData = CompilerInfo->GetFunctionData();
	if (FunctionData.FunctionFlags & (FUNC_NetRequest | FUNC_NetResponse))
	{
		CurrentFunctionText.Logf(TEXT("\t\t\tReturnFunction->RPCId=%d;\r\n"), FunctionData.RPCId);
		CurrentFunctionText.Logf(TEXT("\t\t\tReturnFunction->RPCResponseId=%d;\r\n"), FunctionData.RPCResponseId);
	}

	CurrentFunctionText.Logf(TEXT("\t\t\tReturnFunction->Bind();\r\n"));
	CurrentFunctionText.Logf(TEXT("\t\t\tReturnFunction->StaticLink();\r\n"));

	if (Meta.Len())
	{
		CurrentFunctionText.Logf(TEXT("#if WITH_METADATA\r\n"));
		CurrentFunctionText.Logf(TEXT("\t\t\tUMetaData* MetaData = ReturnFunction->GetOutermost()->GetMetaData();\r\n"));
		CurrentFunctionText.Log(*Meta);
		CurrentFunctionText.Logf(TEXT("#endif\r\n"));
	}

	CurrentFunctionText.Logf(TEXT("\t\t}\r\n"));
	CurrentFunctionText.Logf(TEXT("\t\treturn ReturnFunction;\r\n"));
	CurrentFunctionText.Logf(TEXT("\t}\r\n"));

	uint32 FunctionCrc = GenerateTextCRC(*CurrentFunctionText);
	GGeneratedCodeCRCs.Add(Function, FunctionCrc);
	UHTMakefile.AddGeneratedCodeCRC(&SourceFile, Function, FunctionCrc);
	Out.Log(CurrentFunctionText);
}

void FNativeClassHeaderGenerator::ExportNatives(FOutputDevice& Out, FClass* Class)
{
	const TCHAR* ClassCPPName = NameLookupCPP.GetNameCPP(Class);
	FString TypeName = Class->HasAnyClassFlags(CLASS_Interface) ? *FString::Printf(TEXT("I%s"), *Class->GetName()) : ClassCPPName;

	Out.Logf(TEXT("\tvoid %s::StaticRegisterNatives%s()\r\n"), ClassCPPName, ClassCPPName);
	Out.Log(TEXT("\t{\r\n"));

	{
		TArray<TTuple<UFunction*, FString>> AnsiNamedFunctionsToExport;
		TArray<TTuple<UFunction*, FString>> TCharNamedFunctionsToExport;
		for (UFunction* Function : TFieldRange<UFunction>(Class, EFieldIteratorFlags::ExcludeSuper))
		{
			if ((Function->FunctionFlags & (FUNC_Native | FUNC_NetRequest)) == FUNC_Native)
			{
				FString OverriddenName = FNativeClassHeaderGenerator::GetOverriddenNameForLiteral(Function);
				if (OverriddenName.StartsWith(TEXT("TEXT(")))
				{
					TCharNamedFunctionsToExport.Emplace(Function, MoveTemp(OverriddenName));
				}
				else
				{
					AnsiNamedFunctionsToExport.Emplace(Function, MoveTemp(OverriddenName));
				}
			}
		}

		Algo::SortBy(AnsiNamedFunctionsToExport,  [](const TTuple<UFunction*, FString>& Pair){ return Pair.Get<0>()->GetFName(); });
		Algo::SortBy(TCharNamedFunctionsToExport, [](const TTuple<UFunction*, FString>& Pair){ return Pair.Get<0>()->GetFName(); });

		if (AnsiNamedFunctionsToExport.Num() > 0 || TCharNamedFunctionsToExport.Num() > 0)
		{
			Out.Logf(TEXT("\t\tUClass* Class = %s::StaticClass();\r\n"), ClassCPPName);
		}

		if (AnsiNamedFunctionsToExport.Num())
		{
			Out.Log(TEXT("\t\tstatic const TNameNativePtrPair<ANSICHAR> AnsiFuncs[] = {\r\n"));

			for (const TTuple<UFunction*, FString>& Func : AnsiNamedFunctionsToExport)
			{
				Out.Logf(
					TEXT("\t\t\t{ %s, (Native)&%s::exec%s },\r\n"),
					*Func.Get<1>(),
					*TypeName,
					*Func.Get<0>()->GetName()
				);
			}

			Out.Log(TEXT("\t\t};\r\n"));
			Out.Logf(TEXT("\t\tFNativeFunctionRegistrar::RegisterFunctions(Class, AnsiFuncs, %d);\r\n"), AnsiNamedFunctionsToExport.Num());
		}

		if (TCharNamedFunctionsToExport.Num())
		{
			Out.Log(TEXT("\t\tstatic const TNameNativePtrPair<TCHAR> TCharFuncs[] = {\r\n"));

			for (const TTuple<UFunction*, FString>& Func : TCharNamedFunctionsToExport)
			{
				Out.Logf(
					TEXT("\t\t\t{ %s, (Native)&%s::exec%s },\r\n"),
					*Func.Get<1>(),
					*TypeName,
					*Func.Get<0>()->GetName()
				);
			}

			Out.Log(TEXT("\t\t};\r\n"));
			Out.Logf(TEXT("\t\tFNativeFunctionRegistrar::RegisterFunctions(Class, TCharFuncs, %d);\r\n"), TCharNamedFunctionsToExport.Num());
		}
	}

	for (UScriptStruct* Struct : TFieldRange<UScriptStruct>(Class, EFieldIteratorFlags::ExcludeSuper))
	{
		if (Struct->StructFlags & STRUCT_Native)
		{
			Out.Logf( TEXT("\t\tUScriptStruct::DeferCppStructOps(FName(TEXT(\"%s\")),new UScriptStruct::TCppStructOps<%s%s>);\r\n"), *Struct->GetName(), Struct->GetPrefixCPP(), *Struct->GetName() );
		}
	}

	Out.Logf(TEXT("\t}\r\n"));
}

void FNativeClassHeaderGenerator::ExportInterfaceCallFunctions(FOutputDevice& OutCpp, FUHTStringBuilder& Out, const TArray<UFunction*>& CallbackFunctions, const TCHAR* ClassName)
{
	FString APIString = GetAPIString();

	for (UFunction* Function : CallbackFunctions)
	{
		FString FunctionName = Function->GetName();

		auto* CompilerInfo = FFunctionData::FindForFunction(Function);

		const FFuncInfo& FunctionData = CompilerInfo->GetFunctionData();
		const TCHAR* ConstQualifier = FunctionData.FunctionReference->HasAllFunctionFlags(FUNC_Const) ? TEXT("const ") : TEXT("");
		FString ExtraParam = FString::Printf(TEXT("%sUObject* O"), ConstQualifier);

		ExportNativeFunctionHeader(Out, ForwardDeclarations, FunctionData, EExportFunctionType::Interface, EExportFunctionHeaderStyle::Declaration, *ExtraParam, *APIString);
		Out.Logf( TEXT(";") LINE_TERMINATOR );

		FString FunctionNameName = FString::Printf(TEXT("NAME_%s_%s"), NameLookupCPP.GetNameCPP(CastChecked<UStruct>(Function->GetOuter())), *FunctionName);
		OutCpp.Logf(TEXT("\tstatic FName %s = FName(TEXT(\"%s\"));") LINE_TERMINATOR, *FunctionNameName, *GetOverriddenFName(Function).ToString());

		ExportNativeFunctionHeader(OutCpp, ForwardDeclarations, FunctionData, EExportFunctionType::Interface, EExportFunctionHeaderStyle::Definition, *ExtraParam, *APIString);
		OutCpp.Logf( LINE_TERMINATOR TEXT("\t{") LINE_TERMINATOR );

		OutCpp.Logf(TEXT("\t\tcheck(O != NULL);") LINE_TERMINATOR);
		OutCpp.Logf(TEXT("\t\tcheck(O->GetClass()->ImplementsInterface(U%s::StaticClass()));") LINE_TERMINATOR, ClassName);

		auto Parameters = GetFunctionParmsAndReturn(FunctionData.FunctionReference);

		// See if we need to create Parms struct
		const bool bHasParms = Parameters.HasParms();
		if (bHasParms)
		{
			FString EventParmStructName = GetEventStructParamsName(Function->GetOuter(), *FunctionName);
			OutCpp.Logf(TEXT("\t\t%s Parms;") LINE_TERMINATOR, *EventParmStructName);
		}

		OutCpp.Logf(TEXT("\t\tUFunction* const Func = O->FindFunction(%s);") LINE_TERMINATOR, *FunctionNameName);
		OutCpp.Log(TEXT("\t\tif (Func)") LINE_TERMINATOR);
		OutCpp.Log(TEXT("\t\t{") LINE_TERMINATOR);

		// code to populate Parms struct
		for (auto It = Parameters.Parms.CreateConstIterator(); It; ++It)
		{
			UProperty* Param = *It;

			OutCpp.Logf(TEXT("\t\t\tParms.%s=%s;") LINE_TERMINATOR, *Param->GetName(), *Param->GetName());
		}

		const FString ObjectRef = FunctionData.FunctionReference->HasAllFunctionFlags(FUNC_Const) ? FString::Printf(TEXT("const_cast<UObject*>(O)")) : TEXT("O");
		OutCpp.Logf(TEXT("\t\t\t%s->ProcessEvent(Func, %s);") LINE_TERMINATOR, *ObjectRef, bHasParms ? TEXT("&Parms") : TEXT("NULL"));

		for (auto It = Parameters.Parms.CreateConstIterator(); It; ++It)
		{
			UProperty* Param = *It;

			if( Param->HasAllPropertyFlags(CPF_OutParm) && !Param->HasAnyPropertyFlags(CPF_ConstParm|CPF_ReturnParm))
			{
				OutCpp.Logf(TEXT("\t\t\t%s=Parms.%s;") LINE_TERMINATOR, *Param->GetName(), *Param->GetName());
			}
		}

		OutCpp.Log(TEXT("\t\t}") LINE_TERMINATOR);
		

		// else clause to call back into native if it's a BlueprintNativeEvent
		if (Function->FunctionFlags & FUNC_Native)
		{
			OutCpp.Logf(TEXT("\t\telse if (auto I = (%sI%s*)(O->GetNativeInterfaceAddress(U%s::StaticClass())))") LINE_TERMINATOR, ConstQualifier, ClassName, ClassName);
			OutCpp.Log(TEXT("\t\t{") LINE_TERMINATOR);

			OutCpp.Log(TEXT("\t\t\t"));
			if (Parameters.Return)
			{
				OutCpp.Log(TEXT("Parms.ReturnValue = "));
			}

			OutCpp.Logf(TEXT("I->%s_Implementation("), *FunctionName);

			bool First = true;
			for (auto It = Parameters.Parms.CreateConstIterator(); It; ++It)
			{
				UProperty* Param = *It;

				if (!First)
				{
					OutCpp.Logf(TEXT(","));
				}
				First = false;

				OutCpp.Logf(TEXT("%s"), *Param->GetName());
			}

			OutCpp.Logf(TEXT(");") LINE_TERMINATOR);

			OutCpp.Logf(TEXT("\t\t}") LINE_TERMINATOR);
		}

		if (Parameters.Return)
		{
			OutCpp.Logf(TEXT("\t\treturn Parms.ReturnValue;") LINE_TERMINATOR);
		}

		OutCpp.Logf(TEXT("\t}") LINE_TERMINATOR);
	}
}

/**
 * Gets preprocessor string to emit GENERATED_U*_BODY() macro is deprecated.
 *
 * @param MacroName Name of the macro to be deprecated.
 *
 * @returns Preprocessor string to emit the message.
 */
FString GetGeneratedMacroDeprecationWarning(const TCHAR* MacroName)
{
	// Deprecation warning is disabled right now. After people get familiar with the new macro it should be re-enabled.
	//return FString() + TEXT("EMIT_DEPRECATED_WARNING_MESSAGE(\"") + MacroName + TEXT("() macro is deprecated. Please use GENERATED_BODY() macro instead.\")") LINE_TERMINATOR;
	return TEXT("");
}

/**
 * Returns a string with access specifier that was met before parsing GENERATED_BODY() macro to preserve it.
 *
 * @param Class Class for which to return the access specifier.
 *
 * @returns Access specifier string.
 */
FString GetPreservedAccessSpecifierString(FClass* Class)
{
	FString PreservedAccessSpecifier;
	if (FClassMetaData* Data = GScriptHelper.FindClassData(Class))
	{
		switch (Data->GeneratedBodyMacroAccessSpecifier)
		{
		case EAccessSpecifier::ACCESS_Private:
			PreservedAccessSpecifier = "private:";
			break;
		case EAccessSpecifier::ACCESS_Protected:
			PreservedAccessSpecifier = "protected:";
			break;
		case EAccessSpecifier::ACCESS_Public:
			PreservedAccessSpecifier = "public:";
			break;
		case EAccessSpecifier::ACCESS_NotAnAccessSpecifier :
			PreservedAccessSpecifier = FString::Printf(TEXT("static_assert(false, \"Unknown access specifier for GENERATED_BODY() macro in class %s.\");"), *GetNameSafe(Class));
			break;
		}
	}

	return PreservedAccessSpecifier + LINE_TERMINATOR;
}

void WriteMacro(FOutputDevice& Output, const FString& MacroName, const FString& MacroContent)
{
	Output.Log(*Macroize(*MacroName, *MacroContent));
}

/**
 * Writes to output device auto-includes for the given source file.
 *
 * @param Out Output device.
 * @param SourceFile Source file.
 */
void ExportAutoIncludes(FOutputDevice& Out, const FUnrealSourceFile& SourceFile)
{
	for (const FHeaderProvider& Include : SourceFile.GetIncludes())
	{
		if (!Include.IsAutoInclude())
		{
			continue;
		}

		const FUnrealSourceFile* AutoIncludedSourceFile = Include.GetResolved();

		if (AutoIncludedSourceFile == nullptr)
		{
			continue;
		}

		Out.Logf(
			TEXT("#ifndef %s")			LINE_TERMINATOR
			TEXT("	#include \"%s\"")	LINE_TERMINATOR
			TEXT("#endif")				LINE_TERMINATOR
			LINE_TERMINATOR,
			*AutoIncludedSourceFile->GetFileDefineName(), *AutoIncludedSourceFile->GetIncludePath());
	}
}

static FString PrivatePropertiesOffsetGetters(const UStruct* Struct, const FString& StructCppName)
{
	check(Struct);

	FUHTStringBuilder Result;
	for (const UProperty* Property : TFieldRange<UProperty>(Struct, EFieldIteratorFlags::ExcludeSuper))
	{
		if (Property && Property->HasAnyPropertyFlags(CPF_NativeAccessSpecifierPrivate | CPF_NativeAccessSpecifierProtected) && !Property->HasAnyPropertyFlags(CPF_EditorOnly))
		{
			const UBoolProperty* BoolProperty = Cast<const UBoolProperty>(Property);
			if (BoolProperty && !BoolProperty->IsNativeBool()) // if it's a bitfield
			{
				continue;
			}

			FString PropertyName = Property->GetName();
			if (Property->HasAllPropertyFlags(CPF_Deprecated))
			{
				PropertyName += TEXT("_DEPRECATED");
			}
			Result.Logf(TEXT("\tFORCEINLINE static uint32 __PPO__%s() { return STRUCT_OFFSET(%s, %s); }") LINE_TERMINATOR,
				*PropertyName, *StructCppName, *PropertyName);
		}
	}

	return Result;
}

void FNativeClassHeaderGenerator::ExportClassFromSourceFileInner(
	FOutputDevice&           OutGeneratedHeaderText,
	FOutputDevice&           OutCpp,
	FOutputDevice&           OutDeclarations,
	FClass*                  Class,
	const FUnrealSourceFile& SourceFile
)
{
	FUHTStringBuilder StandardUObjectConstructorsMacroCall;
	FUHTStringBuilder EnhancedUObjectConstructorsMacroCall;

	FClassMetaData* ClassData = GScriptHelper.FindClassData(Class);
	check(ClassData);

	// C++ -> VM stubs (native function execs)
	FUHTStringBuilder ClassMacroCalls;
	FUHTStringBuilder ClassNoPureDeclsMacroCalls;
	ExportNativeFunctions(OutGeneratedHeaderText, ClassMacroCalls, ClassNoPureDeclsMacroCalls, SourceFile, Class, ClassData);

	// Get Callback functions
	TArray<UFunction*> CallbackFunctions;
	{
		for (UFunction* Function : TFieldRange<UFunction>(Class, EFieldIteratorFlags::ExcludeSuper))
		{
			if ((Function->FunctionFlags & FUNC_Event) && Function->GetSuperFunction() == nullptr)
			{
				CallbackFunctions.Add(Function);
			}
		}
	}

	FUHTStringBuilder PrologMacroCalls;
	if (CallbackFunctions.Num() != 0)
	{
		Algo::SortBy(CallbackFunctions, [](UObject* Obj) { return Obj->GetName(); });

		FUHTStringBuilder UClassMacroContent;

		// export parameters structs for all events and delegates
		for (UFunction* Function : CallbackFunctions)
		{
			ExportEventParm(UClassMacroContent, ForwardDeclarations, Function, /*Indent=*/ 1, /*bOutputConstructor=*/ true, EExportingState::Normal);
		}

		FString MacroName = SourceFile.GetGeneratedMacroName(ClassData, TEXT("_EVENT_PARMS"));
		WriteMacro(OutGeneratedHeaderText, MacroName, UClassMacroContent);
		PrologMacroCalls.Logf(TEXT("\t%s\r\n"), *MacroName);

		// VM -> C++ proxies (events and delegates).
		FUHTStringBuilder NullOutput;
		FOutputDevice& CallbackOut = Class->HasAnyClassFlags(CLASS_NoExport) ? NullOutput : OutCpp;
		FString CallbackWrappersMacroName = SourceFile.GetGeneratedMacroName(ClassData, TEXT("_CALLBACK_WRAPPERS"));
		ExportCallbackFunctions(
			OutGeneratedHeaderText,
			CallbackOut,
			ForwardDeclarations,
			CallbackFunctions,
			*CallbackWrappersMacroName,
			(Class->ClassFlags & CLASS_Interface) ? EExportCallbackType::Interface : EExportCallbackType::Class,
			*API,
			*GetAPIString()
		);

		ClassMacroCalls.Logf(TEXT("\t%s\r\n"), *CallbackWrappersMacroName);
		ClassNoPureDeclsMacroCalls.Logf(TEXT("\t%s\r\n"), *CallbackWrappersMacroName);
	}

	// Class definition.
	if (!Class->HasAnyClassFlags(CLASS_NoExport))
	{
		ExportNatives(OutCpp, Class);
	}

	FUHTStringBuilder FriendText;
	ExportNativeGeneratedInitCode(OutCpp, OutDeclarations, SourceFile, Class, FriendText);

	FClass* SuperClass = Class->GetSuperClass();

	// the name for the C++ version of the UClass
	const TCHAR* ClassCPPName = NameLookupCPP.GetNameCPP(Class);
	const TCHAR* SuperClassCPPName = (SuperClass != nullptr) ? NameLookupCPP.GetNameCPP(SuperClass) : nullptr;

	FString APIArg = API;
	if (!Class->HasAnyClassFlags(CLASS_MinimalAPI))
	{
		APIArg = TEXT("NO");
	}

	FString PPOMacroName;

	// Replication, add in the declaration for GetLifetimeReplicatedProps() automatically if there are any net flagged properties
	bool bNeedsRep = false;
	for (TFieldIterator<UProperty> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if ((It->PropertyFlags & CPF_Net) != 0)
		{
			bNeedsRep = true;
			break;
		}
	}

	ClassDefinitionRange ClassRange;
	if (ClassDefinitionRange* FoundRange = ClassDefinitionRanges.Find(Class))
	{
		ClassRange = *FoundRange;
		ClassRange.Validate();
	}

	bool bHasGetLifetimeReplicatedProps = HasIdentifierExactMatch(ClassRange.Start, ClassRange.End, TEXT("GetLifetimeReplicatedProps"));

	{
		FUHTStringBuilder Boilerplate;

		// Export the class's native function registration.
		Boilerplate.Logf(TEXT("private:\r\n"));
		Boilerplate.Logf(TEXT("\tstatic void StaticRegisterNatives%s();\r\n"), ClassCPPName);
		Boilerplate.Log(*FriendText);
		Boilerplate.Logf(TEXT("public:\r\n"));

		const bool bCastedClass = Class->HasAnyCastFlag(CASTCLASS_AllFlags) && SuperClass && Class->ClassCastFlags != SuperClass->ClassCastFlags;

		Boilerplate.Logf(TEXT("\tDECLARE_CLASS(%s, %s, COMPILED_IN_FLAGS(%s%s), %s, TEXT(\"%s\"), %s_API)\r\n"),
			ClassCPPName,
			SuperClassCPPName ? SuperClassCPPName : TEXT("None"),
			Class->HasAnyClassFlags(CLASS_Abstract) ? TEXT("CLASS_Abstract") : TEXT("0"),
			*GetClassFlagExportText(Class),
			bCastedClass ? *FString::Printf(TEXT("CASTCLASS_%s"), ClassCPPName) : TEXT("0"),
			*FClass::GetTypePackageName(Class),
			*APIArg);

		Boilerplate.Logf(TEXT("\tDECLARE_SERIALIZER(%s)\r\n"), ClassCPPName);
		Boilerplate.Log(TEXT("\tenum {IsIntrinsic=COMPILED_IN_INTRINSIC};\r\n"));

		if (SuperClass && Class->ClassWithin != SuperClass->ClassWithin)
		{
			Boilerplate.Logf(TEXT("\tDECLARE_WITHIN(%s)\r\n"), NameLookupCPP.GetNameCPP(Class->GetClassWithin()));
		}

		if (Class->HasAnyClassFlags(CLASS_Interface))
		{
			ExportConstructorsMacros(OutGeneratedHeaderText, OutCpp, StandardUObjectConstructorsMacroCall, EnhancedUObjectConstructorsMacroCall, SourceFile.GetGeneratedMacroName(ClassData), Class, *APIArg);

			OutGeneratedHeaderText.Log(TEXT("#undef GENERATED_UINTERFACE_BODY_COMMON\r\n"));
			OutGeneratedHeaderText.Log(Macroize(TEXT("GENERATED_UINTERFACE_BODY_COMMON()"), *Boilerplate));

			int32 ClassGeneratedBodyLine = ClassData->GetGeneratedBodyLine();

			FString DeprecationWarning = GetGeneratedMacroDeprecationWarning(TEXT("GENERATED_UINTERFACE_BODY"));

			const TCHAR* DeprecationPushString = TEXT("PRAGMA_DISABLE_DEPRECATION_WARNINGS") LINE_TERMINATOR;
			const TCHAR* DeprecationPopString = TEXT("PRAGMA_ENABLE_DEPRECATION_WARNINGS") LINE_TERMINATOR;
			const TCHAR* Offset = TEXT("\t");

			OutGeneratedHeaderText.Logf(
				TEXT("%s"),
				*Macroize(
					*SourceFile.GetGeneratedBodyMacroName(ClassGeneratedBodyLine, true),
					*(
						FString() +
						Offset + DeprecationWarning +
						Offset + DeprecationPushString +
						Offset + TEXT("GENERATED_UINTERFACE_BODY_COMMON()") LINE_TERMINATOR +
						StandardUObjectConstructorsMacroCall +
						Offset + DeprecationPopString
					)
				)
			);

			OutGeneratedHeaderText.Logf(
				TEXT("%s"),
				*Macroize(
					*SourceFile.GetGeneratedBodyMacroName(ClassGeneratedBodyLine),
					*(
						FString() +
						Offset + DeprecationPushString +
						Offset + TEXT("GENERATED_UINTERFACE_BODY_COMMON()") LINE_TERMINATOR +
						EnhancedUObjectConstructorsMacroCall +
						GetPreservedAccessSpecifierString(Class) +
						Offset + DeprecationPopString
					)
				)
			);

			// =============================================
			// Export the pure interface version of the class

			// the name of the pure interface class
			FString InterfaceCPPName = FString::Printf(TEXT("I%s"), *Class->GetName());
			FString SuperInterfaceCPPName;
			if (SuperClass != NULL)
			{
				SuperInterfaceCPPName = FString::Printf(TEXT("I%s"), *SuperClass->GetName());
			}

			// Thunk functions
			FUHTStringBuilder InterfaceBoilerplate;

			InterfaceBoilerplate.Logf(TEXT("protected:\r\n\tvirtual ~%s() {}\r\npublic:\r\n"), *InterfaceCPPName);
			InterfaceBoilerplate.Logf(TEXT("\ttypedef %s UClassType;\r\n"), ClassCPPName);

			ExportInterfaceCallFunctions(OutCpp, InterfaceBoilerplate, CallbackFunctions, *Class->GetName());

			// we'll need a way to get to the UObject portion of a native interface, so that we can safely pass native interfaces
			// to script VM functions
			if (SuperClass->IsChildOf(UInterface::StaticClass()))
			{
				InterfaceBoilerplate.Logf(TEXT("\tvirtual UObject* _getUObject() const = 0;\r\n"));
			}

			if (bNeedsRep && !bHasGetLifetimeReplicatedProps)
			{
				if (SourceFile.GetGeneratedCodeVersionForStruct(Class) == EGeneratedCodeVersion::V1)
				{
					InterfaceBoilerplate.Logf(TEXT("\tvoid GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;\r\n"));
				}
				else
				{
					FError::Throwf(TEXT("Class %s has Net flagged properties and should declare member function: void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override"), ClassCPPName);
				}
			}

			FString NoPureDeclsMacroName = SourceFile.GetGeneratedMacroName(ClassData, TEXT("_INCLASS_IINTERFACE_NO_PURE_DECLS"));
			WriteMacro(OutGeneratedHeaderText, NoPureDeclsMacroName, InterfaceBoilerplate);
			ClassNoPureDeclsMacroCalls.Logf(TEXT("\t%s\r\n"), *NoPureDeclsMacroName);

			FString MacroName = SourceFile.GetGeneratedMacroName(ClassData, TEXT("_INCLASS_IINTERFACE"));
			WriteMacro(OutGeneratedHeaderText, MacroName, InterfaceBoilerplate);
			ClassMacroCalls.Logf(TEXT("\t%s\r\n"), *MacroName);
		}
		else
		{
			// export the class's config name
			if (SuperClass && Class->ClassConfigName != NAME_None && Class->ClassConfigName != SuperClass->ClassConfigName)
			{
				Boilerplate.Logf(TEXT("\tstatic const TCHAR* StaticConfigName() {return TEXT(\"%s\");}\r\n\r\n"), *Class->ClassConfigName.ToString());
			}

			// export implementation of _getUObject for classes that implement interfaces
			if (Class->Interfaces.Num() > 0)
			{
				Boilerplate.Logf(TEXT("\tvirtual UObject* _getUObject() const override { return const_cast<%s*>(this); }\r\n"), ClassCPPName);
			}

			if (bNeedsRep && !bHasGetLifetimeReplicatedProps)
			{
				// Default version autogenerates declarations.
				if (SourceFile.GetGeneratedCodeVersionForStruct(Class) == EGeneratedCodeVersion::V1)
				{
					Boilerplate.Logf(TEXT("\tvoid GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;\r\n"));
				}
				else
				{
					FError::Throwf(TEXT("Class %s has Net flagged properties and should declare member function: void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override"), ClassCPPName);
				}
			}
			{
				FString NoPureDeclsMacroName = SourceFile.GetGeneratedMacroName(ClassData, TEXT("_INCLASS_NO_PURE_DECLS"));
				WriteMacro(OutGeneratedHeaderText, NoPureDeclsMacroName, Boilerplate);
				ClassNoPureDeclsMacroCalls.Logf(TEXT("\t%s\r\n"), *NoPureDeclsMacroName);

				FString MacroName = SourceFile.GetGeneratedMacroName(ClassData, TEXT("_INCLASS"));
				WriteMacro(OutGeneratedHeaderText, MacroName, Boilerplate);
				ClassMacroCalls.Logf(TEXT("\t%s\r\n"), *MacroName);

				ExportConstructorsMacros(OutGeneratedHeaderText, OutCpp, StandardUObjectConstructorsMacroCall, EnhancedUObjectConstructorsMacroCall, SourceFile.GetGeneratedMacroName(ClassData), Class, *APIArg);
			}
			{
				const FString PrivatePropertiesOffsets = PrivatePropertiesOffsetGetters(Class, ClassCPPName);
				const FString PPOMacroNameRaw = SourceFile.GetGeneratedMacroName(ClassData, TEXT("_PRIVATE_PROPERTY_OFFSET"));
				PPOMacroName = FString::Printf(TEXT("\t%s\r\n"), *PPOMacroNameRaw);
				WriteMacro(OutGeneratedHeaderText, PPOMacroNameRaw, PrivatePropertiesOffsets);
			}
		}
	}

	{
		FString MacroName = SourceFile.GetGeneratedMacroName(ClassData->GetPrologLine(), TEXT("_PROLOG"));
		WriteMacro(OutGeneratedHeaderText, MacroName, PrologMacroCalls);
	}

	{
		bool bIsIInterface = Class->HasAnyClassFlags(CLASS_Interface);

		auto MacroName = FString::Printf(TEXT("GENERATED_%s_BODY()"), bIsIInterface ? TEXT("IINTERFACE") : TEXT("UCLASS"));

		auto DeprecationWarning = bIsIInterface ? FString(TEXT("")) : GetGeneratedMacroDeprecationWarning(*MacroName);

		auto DeprecationPushString = TEXT("PRAGMA_DISABLE_DEPRECATION_WARNINGS") LINE_TERMINATOR;
		auto DeprecationPopString = TEXT("PRAGMA_ENABLE_DEPRECATION_WARNINGS") LINE_TERMINATOR;

		auto Public = TEXT("public:" LINE_TERMINATOR);

		auto GeneratedBodyLine = bIsIInterface ? ClassData->GetInterfaceGeneratedBodyLine() : ClassData->GetGeneratedBodyLine();
		auto LegacyGeneratedBody = FString(bIsIInterface ? TEXT("") : PPOMacroName)
			+ ClassMacroCalls 
			+ (bIsIInterface ? TEXT("") : StandardUObjectConstructorsMacroCall);
		auto GeneratedBody = FString(bIsIInterface ? TEXT("") : PPOMacroName)
			+ ClassNoPureDeclsMacroCalls 
			+ (bIsIInterface ? TEXT("") : EnhancedUObjectConstructorsMacroCall);

		auto WrappedLegacyGeneratedBody = DeprecationWarning + DeprecationPushString + Public + LegacyGeneratedBody + Public + DeprecationPopString;
		auto WrappedGeneratedBody = FString(DeprecationPushString) + Public + GeneratedBody + GetPreservedAccessSpecifierString(Class) + DeprecationPopString;

		auto BodyMacros = Macroize(*SourceFile.GetGeneratedBodyMacroName(GeneratedBodyLine, true), *WrappedLegacyGeneratedBody) +
			Macroize(*SourceFile.GetGeneratedBodyMacroName(GeneratedBodyLine, false), *WrappedGeneratedBody);

		OutGeneratedHeaderText.Log(*BodyMacros);
	}
}

/**
* Generates private copy-constructor declaration.
*
* @param Out Output device to generate to.
* @param Class Class to generate constructor for.
* @param API API string for this constructor.
*/
void ExportCopyConstructorDefinition(FOutputDevice& Out, const TCHAR* API, const TCHAR* ClassCPPName)
{
	Out.Logf(TEXT("private:\r\n"));
	Out.Logf(TEXT("\t/** Private move- and copy-constructors, should never be used */\r\n"));
	Out.Logf(TEXT("\t%s_API %s(%s&&);\r\n"), API, ClassCPPName, ClassCPPName);
	Out.Logf(TEXT("\t%s_API %s(const %s&);\r\n"), API, ClassCPPName, ClassCPPName);
	Out.Logf(TEXT("public:\r\n"));
}

/**
 * Generates vtable helper caller and eventual constructor body.
 *
 * @param Out Output device to generate to.
 * @param Class Class to generate for.
 * @param API API string.
 */
void ExportVTableHelperCtorAndCaller(FOutputDevice& Out, FClassMetaData* ClassData, const TCHAR* API, const TCHAR* ClassCPPName)
{
	if (!ClassData->bCustomVTableHelperConstructorDeclared)
	{
		Out.Logf(TEXT("\tDECLARE_VTABLE_PTR_HELPER_CTOR(%s_API, %s);" LINE_TERMINATOR), API, ClassCPPName);
	}
	Out.Logf(TEXT("DEFINE_VTABLE_PTR_HELPER_CTOR_CALLER(%s);" LINE_TERMINATOR), ClassCPPName);
}

/**
 * Generates standard constructor declaration.
 *
 * @param Out Output device to generate to.
 * @param Class Class to generate constructor for.
 * @param API API string for this constructor.
 */
void ExportStandardConstructorsMacro(FOutputDevice& Out, FClass* Class, FClassMetaData* ClassData, const TCHAR* API, const TCHAR* ClassCPPName)
{
	if (!Class->HasAnyClassFlags(CLASS_CustomConstructor))
	{
		Out.Logf(TEXT("\t/** Standard constructor, called after all reflected properties have been initialized */\r\n"));
		Out.Logf(TEXT("\t%s_API %s(const FObjectInitializer& ObjectInitializer%s);\r\n"), API, ClassCPPName,
			ClassData->bDefaultConstructorDeclared ? TEXT("") : TEXT(" = FObjectInitializer::Get()"));
	}
	Out.Logf(TEXT("\tDEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(%s)\r\n"), ClassCPPName);

	ExportVTableHelperCtorAndCaller(Out, ClassData, API, ClassCPPName);
	ExportCopyConstructorDefinition(Out, API, ClassCPPName);
}

/**
 * Generates constructor definition.
 *
 * @param Out Output device to generate to.
 * @param Class Class to generate constructor for.
 * @param API API string for this constructor.
 */
void ExportConstructorDefinition(FOutputDevice& Out, FClass* Class, FClassMetaData* ClassData, const TCHAR* API, const TCHAR* ClassCPPName)
{
	if (!ClassData->bConstructorDeclared)
	{
		Out.Logf(TEXT("\t/** Standard constructor, called after all reflected properties have been initialized */\r\n"));
		
		// Assume super class has OI constructor, this may not always be true but we should always be able to check this.
		// In any case, it will default to old behaviour before we even checked this.
		bool bSuperClassObjectInitializerConstructorDeclared = true;
		FClass* SuperClass = Class->GetSuperClass();
		if (SuperClass != nullptr)
		{
			FClassMetaData* SuperClassData = GScriptHelper.FindClassData(SuperClass);
			if (SuperClassData)
			{
				bSuperClassObjectInitializerConstructorDeclared = SuperClassData->bObjectInitializerConstructorDeclared;
			}
		}
		if (bSuperClassObjectInitializerConstructorDeclared)
		{
			Out.Logf(TEXT("\t%s_API %s(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get()) : Super(ObjectInitializer) { };\r\n"), API, ClassCPPName);
			ClassData->bObjectInitializerConstructorDeclared = true;
		}
		else
		{
			Out.Logf(TEXT("\t%s_API %s() { };\r\n"), API, ClassCPPName);
			ClassData->bDefaultConstructorDeclared = true;
		}

		ClassData->bConstructorDeclared = true;
	}
	ExportCopyConstructorDefinition(Out, API, ClassCPPName);
}

/**
 * Generates constructor call definition.
 *
 * @param Out Output device to generate to.
 * @param Class Class to generate constructor call definition for.
 */
void ExportDefaultConstructorCallDefinition(FOutputDevice& Out, FClassMetaData* ClassData, const TCHAR* ClassCPPName)
{
	if (ClassData->bObjectInitializerConstructorDeclared)
	{
		Out.Logf(TEXT("\tDEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(%s)\r\n"), ClassCPPName);
	}
	else if (ClassData->bDefaultConstructorDeclared)
	{
		Out.Logf(TEXT("\tDEFINE_DEFAULT_CONSTRUCTOR_CALL(%s)\r\n"), ClassCPPName);
	}
	else
	{
		Out.Logf(TEXT("\tDEFINE_FORBIDDEN_DEFAULT_CONSTRUCTOR_CALL(%s)\r\n"), ClassCPPName);
	}
}

/**
 * Generates enhanced constructor declaration.
 *
 * @param Out Output device to generate to.
 * @param Class Class to generate constructor for.
 * @param API API string for this constructor.
 */
void ExportEnhancedConstructorsMacro(FOutputDevice& Out, FClass* Class, FClassMetaData* ClassData, const TCHAR* API, const TCHAR* ClassCPPName)
{
	ExportConstructorDefinition(Out, Class, ClassData, API, ClassCPPName);
	ExportVTableHelperCtorAndCaller(Out, ClassData, API, ClassCPPName);
	ExportDefaultConstructorCallDefinition(Out, ClassData, ClassCPPName);
}

/**
 * Gets a package relative inclusion path of the given source file for build.
 *
 * @param SourceFile Given source file.
 *
 * @returns Inclusion path.
 */
FString GetBuildPath(FUnrealSourceFile& SourceFile)
{
	FString Out = SourceFile.GetFilename();

	ConvertToBuildIncludePath(SourceFile.GetPackage(), Out);

	return Out;
}

void FNativeClassHeaderGenerator::ExportConstructorsMacros(FOutputDevice& OutGeneratedHeaderText, FOutputDevice& Out, FOutputDevice& StandardUObjectConstructorsMacroCall, FOutputDevice& EnhancedUObjectConstructorsMacroCall, const FString& ConstructorsMacroPrefix, FClass* Class, const TCHAR* APIArg)
{
	const TCHAR* ClassCPPName = NameLookupCPP.GetNameCPP(Class);

	FClassMetaData* ClassData = GScriptHelper.FindClassData(Class);
	check(ClassData);

	FUHTStringBuilder StdMacro;
	FUHTStringBuilder EnhMacro;
	FString StdMacroName = ConstructorsMacroPrefix + TEXT("_STANDARD_CONSTRUCTORS");
	FString EnhMacroName = ConstructorsMacroPrefix + TEXT("_ENHANCED_CONSTRUCTORS");

	ExportStandardConstructorsMacro(StdMacro, Class, ClassData, APIArg, ClassCPPName);
	ExportEnhancedConstructorsMacro(EnhMacro, Class, ClassData, APIArg, ClassCPPName);

	if (!ClassData->bCustomVTableHelperConstructorDeclared)
	{
		Out.Logf(TEXT("\tDEFINE_VTABLE_PTR_HELPER_CTOR(%s);" LINE_TERMINATOR), ClassCPPName);
	}

	OutGeneratedHeaderText.Log(*Macroize(*StdMacroName, *StdMacro));
	OutGeneratedHeaderText.Log(*Macroize(*EnhMacroName, *EnhMacro));

	StandardUObjectConstructorsMacroCall.Logf(TEXT("\t%s\r\n"), *StdMacroName);
	EnhancedUObjectConstructorsMacroCall.Logf(TEXT("\t%s\r\n"), *EnhMacroName);
}

bool FNativeClassHeaderGenerator::WriteHeader(const TCHAR* Path, const FString& InBodyText, const TSet<FString>& InFwdDecl)
{
	FUHTStringBuilder GeneratedHeaderTextWithCopyright;
	GeneratedHeaderTextWithCopyright.Logf(TEXT("%s"), HeaderCopyright);
	GeneratedHeaderTextWithCopyright.Log(TEXT("#include \"ObjectMacros.h\"\r\n"));
	GeneratedHeaderTextWithCopyright.Log(TEXT("#include \"ScriptMacros.h\"\r\n"));
	GeneratedHeaderTextWithCopyright.Log(LINE_TERMINATOR);
	GeneratedHeaderTextWithCopyright.Log(TEXT("PRAGMA_DISABLE_DEPRECATION_WARNINGS") LINE_TERMINATOR);

	for (const FString& FWDecl : InFwdDecl)
	{
		if (FWDecl.Len() > 0)
		{
			GeneratedHeaderTextWithCopyright.Logf(TEXT("%s\r\n"), *FWDecl);
		}
	}

	GeneratedHeaderTextWithCopyright.Log(*InBodyText);
	GeneratedHeaderTextWithCopyright.Log(TEXT("PRAGMA_ENABLE_DEPRECATION_WARNINGS") LINE_TERMINATOR);

	bool bHasChanged = SaveHeaderIfChanged(Path, *GeneratedHeaderTextWithCopyright);
	return bHasChanged;
}

/**
 * Returns a string in the format CLASS_Something|CLASS_Something which represents all class flags that are set for the specified
 * class which need to be exported as part of the DECLARE_CLASS macro
 */
FString FNativeClassHeaderGenerator::GetClassFlagExportText( UClass* Class )
{
	FString StaticClassFlagText;

	check(Class);
	if ( Class->HasAnyClassFlags(CLASS_Transient) )
	{
		StaticClassFlagText += TEXT(" | CLASS_Transient");
	}				
	if( Class->HasAnyClassFlags(CLASS_DefaultConfig) )
	{
		StaticClassFlagText += TEXT(" | CLASS_DefaultConfig");
	}
	if( Class->HasAnyClassFlags(CLASS_GlobalUserConfig) )
	{
		StaticClassFlagText += TEXT(" | CLASS_GlobalUserConfig");
	}
	if( Class->HasAnyClassFlags(CLASS_Config) )
	{
		StaticClassFlagText += TEXT(" | CLASS_Config");
	}
	if ( Class->HasAnyClassFlags(CLASS_Interface) )
	{
		StaticClassFlagText += TEXT(" | CLASS_Interface");
	}
	if ( Class->HasAnyClassFlags(CLASS_Deprecated) )
	{
		StaticClassFlagText += TEXT(" | CLASS_Deprecated");
	}

	return StaticClassFlagText;
}

/**
* Exports the header text for the list of enums specified
*
* @param	Enums	the enums to export
*/
void FNativeClassHeaderGenerator::ExportEnum(FOutputDevice& Out, UEnum* Enum)
{
	// Export FOREACH macro
	Out.Logf( TEXT("#define FOREACH_ENUM_%s(op) "), *Enum->GetName().ToUpper() );
	for (int32 i = 0; i < Enum->NumEnums() - 1; i++)
	{
		const FString QualifiedEnumValue = Enum->GetNameByIndex(i).ToString();
		Out.Logf( TEXT("\\\r\n\top(%s) "), *QualifiedEnumValue );
	}
	Out.Logf( TEXT("\r\n") );
}

// Exports the header text for the list of structs specified (GENERATED_BODY impls)
void FNativeClassHeaderGenerator::ExportGeneratedStructBodyMacros(FOutputDevice& OutGeneratedHeaderText, FOutputDevice& Out, FOutputDevice& OutDeclarations, const FUnrealSourceFile& SourceFile, UScriptStruct* Struct)
{
	const bool bIsDynamic = FClass::IsDynamic(Struct);
	const FString ActualStructName = FNativeClassHeaderGenerator::GetOverriddenName(Struct);

	UStruct* BaseStruct = Struct->GetSuperStruct();

	// Export struct.
	if (Struct->StructFlags & STRUCT_Native)
	{
		check(Struct->StructMacroDeclaredLineNumber != INDEX_NONE);

		const FString FriendApiString = GetAPIString();
		const FString StaticConstructionString = GetSingletonName(Struct);

		FString RequiredAPI;
		if (!(Struct->StructFlags & STRUCT_RequiredAPI))
		{
			RequiredAPI = FriendApiString;
		}

		const TCHAR* StructNameCPP = NameLookupCPP.GetNameCPP(Struct);

		const FString FriendLine = FString::Printf(TEXT("\tfriend %sclass UScriptStruct* %s;\r\n"), *FriendApiString, *StaticConstructionString);
		const FString StaticClassLine = FString::Printf(TEXT("\t%sstatic class UScriptStruct* StaticStruct();\r\n"), *RequiredAPI);
		const FString PrivatePropertiesOffset = PrivatePropertiesOffsetGetters(Struct, StructNameCPP);
		const FString SuperTypedef = BaseStruct ? FString::Printf(TEXT("\ttypedef %s Super;\r\n"), NameLookupCPP.GetNameCPP(BaseStruct)) : FString();

		const FString CombinedLine = FriendLine + StaticClassLine + PrivatePropertiesOffset + SuperTypedef;
		const FString MacroName = SourceFile.GetGeneratedBodyMacroName(Struct->StructMacroDeclaredLineNumber);

		const FString Macroized = Macroize(*MacroName, *CombinedLine);
		OutGeneratedHeaderText.Log(*Macroized);

		FString SingletonName = StaticConstructionString.Replace(TEXT("()"), TEXT(""), ESearchCase::CaseSensitive); // function address
		FString GetCRCName = FString::Printf(TEXT("Get_%s_CRC"), *SingletonName);
		
		Out.Logf(TEXT("class UScriptStruct* %s::StaticStruct()\r\n"), StructNameCPP);
		Out.Logf(TEXT("{\r\n"));

		// UStructs can have UClass or UPackage outer (if declared in non-UClass headers).
		FString OuterName;
		if (Struct->GetOuter()->IsA(UStruct::StaticClass()))
		{
			OuterName = NameLookupCPP.GetNameCPP(CastChecked<UStruct>(Struct->GetOuter()));
			OuterName += TEXT("::StaticClass()");
		}
		else if (!bIsDynamic)
		{
			OuterName = GetPackageSingletonName(CastChecked<UPackage>(Struct->GetOuter()));
			Out.Logf(TEXT("\textern %sclass UPackage* %s;\r\n"), *FriendApiString, *OuterName);
		}
		else
		{
			OuterName = TEXT("StructPackage");
			Out.Logf(TEXT("\tclass UPackage* %s = FindOrConstructDynamicTypePackage(TEXT(\"%s\"));\r\n"), *OuterName, *FClass::GetTypePackageName(Struct));
		}

		if (!bIsDynamic)
		{
			Out.Logf(TEXT("\tstatic class UScriptStruct* Singleton = NULL;\r\n"));
		}
		else
		{
			Out.Logf(TEXT("\tclass UScriptStruct* Singleton = Cast<UScriptStruct>(StaticFindObjectFast(UScriptStruct::StaticClass(), %s, TEXT(\"%s\")));\r\n"),
				*OuterName, *ActualStructName);
		}
		Out.Logf(TEXT("\tif (!Singleton)\r\n"));
		Out.Logf(TEXT("\t{\r\n"));
		Out.Logf(TEXT("\t\textern %sclass UScriptStruct* %s;\r\n"), *FriendApiString, *StaticConstructionString);
		Out.Logf(TEXT("\t\textern %suint32 %s();\r\n"), *FriendApiString, *GetCRCName);

		Out.Logf(TEXT("\t\tSingleton = GetStaticStruct(%s, %s, TEXT(\"%s\"), sizeof(%s), %s());\r\n"),
			*SingletonName, *OuterName, *ActualStructName, StructNameCPP, *GetCRCName);

		Out.Logf(TEXT("\t}\r\n"));
		Out.Logf(TEXT("\treturn Singleton;\r\n"));
		Out.Logf(TEXT("}\r\n"));

		Out.Logf(TEXT("static FCompiledInDeferStruct Z_CompiledInDeferStruct_UScriptStruct_%s(%s::StaticStruct, TEXT(\"%s\"), TEXT(\"%s\"), %s, %s, %s);\r\n"),
			StructNameCPP, StructNameCPP, *Struct->GetOutermost()->GetName(), *ActualStructName,
			bIsDynamic ? TEXT("true") : TEXT("false"),
			bIsDynamic ? *AsTEXT(FClass::GetTypePackageName(Struct)) : TEXT("nullptr"),
			bIsDynamic ? *AsTEXT(FNativeClassHeaderGenerator::GetOverriddenPathName(Struct)) : TEXT("nullptr"));

		// Generate StaticRegisterNatives equivalent for structs without classes.
		if (!Struct->GetOuter()->IsA(UStruct::StaticClass()))
		{
			const FString ShortPackageName = FPackageName::GetShortName(Struct->GetOuter()->GetName());
			Out.Logf(TEXT("static struct FScriptStruct_%s_StaticRegisterNatives%s\r\n"), *ShortPackageName, StructNameCPP);
			Out.Logf(TEXT("{\r\n"));
			Out.Logf(TEXT("\tFScriptStruct_%s_StaticRegisterNatives%s()\r\n"), *ShortPackageName, StructNameCPP);
			Out.Logf(TEXT("\t{\r\n"));

			Out.Logf(TEXT("\t\tUScriptStruct::DeferCppStructOps(FName(TEXT(\"%s\")),new UScriptStruct::TCppStructOps<%s>);\r\n"), *ActualStructName, StructNameCPP);

			Out.Logf(TEXT("\t}\r\n"));
			Out.Logf(TEXT("} ScriptStruct_%s_StaticRegisterNatives%s;\r\n"), *ShortPackageName, StructNameCPP);
		}
	}

	const FString SingletonName = GetSingletonName(Struct);
	OutDeclarations.Log(FTypeSingletonCache::Get(Struct).GetExternDecl());

	FUHTStringBuilder GeneratedStructRegisterFunctionText;

	GeneratedStructRegisterFunctionText.Logf(TEXT("\tUScriptStruct* %s\r\n"), *SingletonName);
	GeneratedStructRegisterFunctionText.Logf(TEXT("\t{\r\n"));

	// if this is a no export struct, we will put a local struct here for offset determination
	TArray<UScriptStruct*> Structs = FindNoExportStructs(Struct);
	for (UScriptStruct* NoExportStruct : Structs)
	{
		ExportMirrorsForNoexportStruct(GeneratedStructRegisterFunctionText, NoExportStruct, /*Indent=*/ 2);
	}

	FString CRCFuncName = FString::Printf(TEXT("Get_%s_CRC"), *SingletonName.Replace(TEXT("()"), TEXT(""), ESearchCase::CaseSensitive));

	// Structs can either have a UClass or UPackage as outer (if declared in non-UClass header).
	if (Struct->GetOuter()->IsA(UStruct::StaticClass()))
	{
		GeneratedStructRegisterFunctionText.Logf(TEXT("\t\tUStruct* Outer = %s;\r\n"), *GetSingletonName(CastChecked<UStruct>(Struct->GetOuter())));
	}
	else if (!bIsDynamic)
	{
		GeneratedStructRegisterFunctionText.Logf(TEXT("\t\tUPackage* Outer = %s;\r\n"), *GetPackageSingletonName(CastChecked<UPackage>(Struct->GetOuter())));
	}
	else
	{
		GeneratedStructRegisterFunctionText.Logf(TEXT("\t\tUPackage* Outer = FindOrConstructDynamicTypePackage(TEXT(\"%s\"));\r\n"), *FClass::GetTypePackageName(Struct));
	}

	GeneratedStructRegisterFunctionText.Logf(TEXT("\t\textern uint32 %s();\r\n"), *CRCFuncName);
	if (!bIsDynamic)
	{
		GeneratedStructRegisterFunctionText.Logf(TEXT("\t\tstatic UScriptStruct* ReturnStruct = FindExistingStructIfHotReloadOrDynamic(Outer, TEXT(\"%s\"), sizeof(%s), %s(), false);\r\n"), *ActualStructName, NameLookupCPP.GetNameCPP(Struct), *CRCFuncName);
	}
	else
	{
		GeneratedStructRegisterFunctionText.Logf(TEXT("\t\tUScriptStruct* ReturnStruct = FindExistingStructIfHotReloadOrDynamic(Outer, TEXT(\"%s\"), sizeof(%s), %s(), true);\r\n"), *ActualStructName, NameLookupCPP.GetNameCPP(Struct), *CRCFuncName);
	}
	GeneratedStructRegisterFunctionText.Logf(TEXT("\t\tif (!ReturnStruct)\r\n"));
	GeneratedStructRegisterFunctionText.Logf(TEXT("\t\t{\r\n"));
	FString BaseStructString(TEXT("NULL"));
	if (BaseStruct)
	{
		CastChecked<UScriptStruct>(BaseStruct); // this better actually be a script struct
		BaseStructString = GetSingletonName(BaseStruct);
	}
	FString CppStructOpsString(TEXT("NULL"));
	FString ExplicitSizeString;
	FString ExplicitAlignmentString;
	if ((Struct->StructFlags&STRUCT_Native) != 0)
	{
		//@todo .u we don't need the auto register versions of these (except for hotreload, which should be fixed)
		CppStructOpsString = FString::Printf(TEXT("new UScriptStruct::TCppStructOps<%s>"), NameLookupCPP.GetNameCPP(Struct));
	}
	else
	{
		ExplicitSizeString = FString::Printf(TEXT(", sizeof(%s), ALIGNOF(%s)"), NameLookupCPP.GetNameCPP(Struct), NameLookupCPP.GetNameCPP(Struct));
	}

	const TCHAR* UStructObjectFlags = bIsDynamic ? TEXT("RF_Public|RF_Transient") : TEXT("RF_Public|RF_Transient|RF_MarkAsNative");
	GeneratedStructRegisterFunctionText.Logf(TEXT("\t\t\tReturnStruct = new(EC_InternalUseOnlyConstructor, Outer, TEXT(\"%s\"), %s) UScriptStruct(FObjectInitializer(), %s, %s, EStructFlags(0x%08X)%s);\r\n"),
		*ActualStructName,
		UStructObjectFlags,
		*BaseStructString,
		*CppStructOpsString,
		(uint32)(Struct->StructFlags & ~STRUCT_ComputedFlags),
		*ExplicitSizeString
		);
	TheFlagAudit.Add(Struct, TEXT("StructFlags"), (uint64)(Struct->StructFlags & ~STRUCT_ComputedFlags));

	TArray<UProperty*> Props;
	for (TFieldIterator<UProperty> ItInner(Struct, EFieldIteratorFlags::ExcludeSuper); ItInner; ++ItInner)
	{
		Props.Add(*ItInner);
	}
	FString OuterString = FString(TEXT("ReturnStruct"));
	FString Meta = GetMetaDataCodeForObject(Struct, *OuterString, TEXT("\t\t\t"));
	OutputProperties(Meta, GeneratedStructRegisterFunctionText, OuterString, Props, TEXT("\t\t\t"));
	GeneratedStructRegisterFunctionText.Logf(TEXT("\t\t\tReturnStruct->StaticLink();\r\n"));

	if (Meta.Len())
	{
		GeneratedStructRegisterFunctionText.Logf(TEXT("#if WITH_METADATA\r\n"));
		GeneratedStructRegisterFunctionText.Logf(TEXT("\t\t\tUMetaData* MetaData = ReturnStruct->GetOutermost()->GetMetaData();\r\n"));
		GeneratedStructRegisterFunctionText.Log(*Meta);
		GeneratedStructRegisterFunctionText.Logf(TEXT("#endif\r\n"));
	}

	GeneratedStructRegisterFunctionText.Logf(TEXT("\t\t}\r\n"));
	GeneratedStructRegisterFunctionText.Logf(TEXT("\t\treturn ReturnStruct;\r\n"));
	GeneratedStructRegisterFunctionText.Logf(TEXT("\t}\r\n"));

	uint32 StructCrc = GenerateTextCRC(*GeneratedStructRegisterFunctionText);
	GGeneratedCodeCRCs.Add(Struct, StructCrc);
	UHTMakefile.AddGeneratedCodeCRC(&SourceFile, Struct, StructCrc);

	Out.Log(GeneratedStructRegisterFunctionText);
	Out.Logf(TEXT("\tuint32 %s() { return %uU; }\r\n"), *CRCFuncName, StructCrc);

	//CallSingletons.Logf(TEXT("\t\t\t\tOuterClass->LinkChild(%s); // %u\r\n"), *SingletonName, StructCrc);
}

void FNativeClassHeaderGenerator::ExportGeneratedEnumInitCode(FOutputDevice& Out, FOutputDevice& OutDeclarations, const FUnrealSourceFile& SourceFile, UEnum* Enum)
{
	const bool bIsDynamic = FClass::IsDynamic(Enum);
	const FString FriendApiString = GetAPIString();
	const FString StaticConstructionString = GetSingletonName(Enum);

	FString SingletonName = StaticConstructionString.Replace(TEXT("()"), TEXT(""), ESearchCase::CaseSensitive); // function address
	FString PackageSingletonName = GetPackageSingletonName(CastChecked<UPackage>(Enum->GetOuter()));
	if (!bIsDynamic)
	{
		PackageSingletonName = GetPackageSingletonName(CastChecked<UPackage>(Enum->GetOuter()));
	}
	else
	{
		PackageSingletonName = FClass::GetTypePackageName(Enum);
	}

	Out.Logf(TEXT("static UEnum* %s_StaticEnum()\r\n"), *Enum->GetName());
	Out.Logf(TEXT("{\r\n"));
	
	if (!bIsDynamic)
	{
		Out.Logf(TEXT("\textern %sclass UPackage* %s;\r\n"), *FriendApiString, *PackageSingletonName);
		Out.Logf(TEXT("\tstatic UEnum* Singleton = nullptr;\r\n"));
	}
	else
	{
		Out.Logf(TEXT("\tclass UPackage* EnumPackage = FindOrConstructDynamicTypePackage(TEXT(\"%s\"));\r\n"), *PackageSingletonName);
		Out.Logf(TEXT("\tclass UEnum* Singleton = Cast<UEnum>(StaticFindObjectFast(UEnum::StaticClass(), EnumPackage, TEXT(\"%s\")));\r\n"), *FNativeClassHeaderGenerator::GetOverriddenName(Enum));
	}
	Out.Logf(TEXT("\tif (!Singleton)\r\n"));
	Out.Logf(TEXT("\t{\r\n"));
	Out.Logf(TEXT("\t\textern %sclass UEnum* %s;\r\n"), *FriendApiString, *StaticConstructionString);
	if (!bIsDynamic)
	{
		Out.Logf(TEXT("\t\tSingleton = GetStaticEnum(%s, %s, TEXT(\"%s\"));\r\n"), *SingletonName, *PackageSingletonName, *Enum->GetName());
	}
	else
	{
		Out.Logf(TEXT("\t\tSingleton = GetStaticEnum(%s, EnumPackage, TEXT(\"%s\"));\r\n"), *SingletonName, *FNativeClassHeaderGenerator::GetOverriddenName(Enum));
	}

	Out.Logf(TEXT("\t}\r\n"));
	Out.Logf(TEXT("\treturn Singleton;\r\n"));
	Out.Logf(TEXT("}\r\n"));

	const FString EnumNameCpp = Enum->GetName(); //UserDefinedEnum should already have a valid cpp name.
	Out.Logf(TEXT("static FCompiledInDeferEnum Z_CompiledInDeferEnum_UEnum_%s(%s_StaticEnum, TEXT(\"%s\"), TEXT(\"%s\"), %s, %s, %s);\r\n"),
		*EnumNameCpp, *EnumNameCpp, *Enum->GetOutermost()->GetName(), *FNativeClassHeaderGenerator::GetOverriddenName(Enum),
		bIsDynamic ? TEXT("true") : TEXT("false"),
		bIsDynamic ? *AsTEXT(FClass::GetTypePackageName(Enum)) : TEXT("nullptr"),
		bIsDynamic ? *AsTEXT(FNativeClassHeaderGenerator::GetOverriddenPathName(Enum)) : TEXT("nullptr"));

	const FString EnumSingletonName = GetSingletonName(Enum);
	OutDeclarations.Log(FTypeSingletonCache::Get(Enum).GetExternDecl());

	FUHTStringBuilder GeneratedEnumRegisterFunctionText;

	FString CRCFuncName = FString::Printf(TEXT("Get_%s_CRC"), *SingletonName.Replace(TEXT("()"), TEXT(""), ESearchCase::CaseSensitive));

	GeneratedEnumRegisterFunctionText.Logf(TEXT("\tUEnum* %s\r\n"), *EnumSingletonName);
	GeneratedEnumRegisterFunctionText.Logf(TEXT("\t{\r\n"));
	// Enums can either have a UClass or UPackage as outer (if declared in non-UClass header).
	if (Enum->GetOuter()->IsA(UStruct::StaticClass()))
	{
		GeneratedEnumRegisterFunctionText.Logf(TEXT("\t\tUClass* Outer=%s;\r\n"), *GetSingletonName(CastChecked<UStruct>(Enum->GetOuter())));
	}
	else if (!bIsDynamic)
	{
		GeneratedEnumRegisterFunctionText.Logf(TEXT("\t\tUPackage* Outer=%s;\r\n"), *GetPackageSingletonName(CastChecked<UPackage>(Enum->GetOuter())));
	}
	else
	{
		GeneratedEnumRegisterFunctionText.Logf(TEXT("\t\tUPackage* Outer = FindOrConstructDynamicTypePackage(TEXT(\"%s\"));\r\n"), *PackageSingletonName);
	}
	GeneratedEnumRegisterFunctionText.Logf(TEXT("\t\textern uint32 %s();\r\n"), *CRCFuncName);
	if (!bIsDynamic)
	{
		GeneratedEnumRegisterFunctionText.Logf(TEXT("\t\tstatic UEnum* ReturnEnum = FindExistingEnumIfHotReloadOrDynamic(Outer, TEXT(\"%s\"), 0, %s(), false);\r\n"), *Enum->GetName(), *CRCFuncName);
	}
	else
	{
		GeneratedEnumRegisterFunctionText.Logf(TEXT("\t\tUEnum* ReturnEnum = FindExistingEnumIfHotReloadOrDynamic(Outer, TEXT(\"%s\"), 0, %s(), true);\r\n"), *FNativeClassHeaderGenerator::GetOverriddenName(Enum), *CRCFuncName);
	}
	GeneratedEnumRegisterFunctionText.Logf(TEXT("\t\tif (!ReturnEnum)\r\n"));
	GeneratedEnumRegisterFunctionText.Logf(TEXT("\t\t{\r\n"));

	const TCHAR* UEnumObjectFlags = bIsDynamic ? TEXT("RF_Public|RF_Transient") : TEXT("RF_Public|RF_Transient|RF_MarkAsNative");
	GeneratedEnumRegisterFunctionText.Logf(TEXT("\t\t\tReturnEnum = new(EC_InternalUseOnlyConstructor, Outer, TEXT(\"%s\"), %s) UEnum(FObjectInitializer());\r\n"), *FNativeClassHeaderGenerator::GetOverriddenName(Enum), UEnumObjectFlags);
	GeneratedEnumRegisterFunctionText.Logf(TEXT("\t\t\tTArray<TPair<FName, int64>> EnumNames;\r\n"));
	for (int32 Index = 0; Index < Enum->NumEnums(); Index++)
	{
		const TCHAR* OverridenNameMetaDatakey = TEXT("OverrideName");
		const FString KeyName = Enum->HasMetaData(OverridenNameMetaDatakey, Index) ? Enum->GetMetaData(OverridenNameMetaDatakey, Index) : Enum->GetNameByIndex(Index).ToString();
		GeneratedEnumRegisterFunctionText.Logf(TEXT("\t\t\tEnumNames.Emplace(TEXT(\"%s\"), %lld);\r\n"), *KeyName, Enum->GetValueByIndex(Index));
	}

	FString EnumTypeStr;
	switch (Enum->GetCppForm())
	{
		case UEnum::ECppForm::Regular:    EnumTypeStr = TEXT("UEnum::ECppForm::Regular");    break;
		case UEnum::ECppForm::Namespaced: EnumTypeStr = TEXT("UEnum::ECppForm::Namespaced"); break;
		case UEnum::ECppForm::EnumClass:  EnumTypeStr = TEXT("UEnum::ECppForm::EnumClass");  break;
	}
	const FString ParamAddMaxKeyIfMissing = FClass::IsDynamic(Enum) ? TEXT(", false") : TEXT("");
	GeneratedEnumRegisterFunctionText.Logf(TEXT("\t\t\tReturnEnum->SetEnums(EnumNames, %s%s);\r\n"), *EnumTypeStr, *ParamAddMaxKeyIfMissing);
	GeneratedEnumRegisterFunctionText.Logf(TEXT("\t\t\tReturnEnum->CppType = TEXT(\"%s\");\r\n"), *Enum->CppType);

	const FString& EnumDisplayNameFn = Enum->GetMetaData(TEXT("EnumDisplayNameFn"));
	if( !EnumDisplayNameFn.IsEmpty() )
	{
		GeneratedEnumRegisterFunctionText.Logf(TEXT("\t\t\tReturnEnum->SetEnumDisplayNameFn(&%s);\r\n"), *EnumDisplayNameFn);
	}

	FString Meta = GetMetaDataCodeForObject(Enum, TEXT("ReturnEnum"), TEXT("\t\t\t"));
	if (Meta.Len())
	{
		GeneratedEnumRegisterFunctionText.Logf(TEXT("#if WITH_METADATA\r\n"));
		GeneratedEnumRegisterFunctionText.Logf(TEXT("\t\t\tUMetaData* MetaData = ReturnEnum->GetOutermost()->GetMetaData();\r\n"));
		GeneratedEnumRegisterFunctionText.Log(*Meta);
		GeneratedEnumRegisterFunctionText.Logf(TEXT("#endif\r\n"));
	}

	GeneratedEnumRegisterFunctionText.Logf(TEXT("\t\t}\r\n"));
	GeneratedEnumRegisterFunctionText.Logf(TEXT("\t\treturn ReturnEnum;\r\n"));
	GeneratedEnumRegisterFunctionText.Logf(TEXT("\t}\r\n"));

	Out.Log(GeneratedEnumRegisterFunctionText);

	uint32 EnumCrc = GenerateTextCRC(*GeneratedEnumRegisterFunctionText);
	GGeneratedCodeCRCs.Add(Enum, EnumCrc);
	UHTMakefile.AddGeneratedCodeCRC(&SourceFile, Enum, EnumCrc);
	Out.Logf(TEXT("\tuint32 %s() { return %uU; }\r\n"), *CRCFuncName, EnumCrc);
	// CallSingletons.Logf(TEXT("\t\t\t\tOuterClass->LinkChild(%s); // %u\r\n"), *EnumSingletonName, EnumCrc);
}

void FNativeClassHeaderGenerator::ExportMirrorsForNoexportStruct(FOutputDevice& Out, UScriptStruct* Struct, int32 TextIndent)
{
	// Export struct.
	const TCHAR* StructName = NameLookupCPP.GetNameCPP(Struct);
	Out.Logf(TEXT("%sstruct %s"), FCString::Tab(TextIndent), StructName);
	if (Struct->GetSuperStruct() != NULL)
	{
		Out.Logf(TEXT(" : public %s"), NameLookupCPP.GetNameCPP(Struct->GetSuperStruct()));
	}
	Out.Logf(TEXT("\r\n%s{\r\n"), FCString::Tab(TextIndent));

	// Export the struct's CPP properties.
	ExportProperties(Out, Struct, TextIndent);

	Out.Logf(TEXT("%s};\r\n\r\n"), FCString::Tab(TextIndent));
}

bool FNativeClassHeaderGenerator::WillExportEventParms( UFunction* Function )
{
  TFieldIterator<UProperty> It(Function);
  return It && (It->PropertyFlags&CPF_Parm);
}

void WriteEventFunctionPrologue(FOutputDevice& Output, int32 Indent, const FParmsAndReturnProperties& Parameters, UObject* FunctionOuter, const TCHAR* FunctionName)
{
	// now the body - first we need to declare a struct which will hold the parameters for the event/delegate call
	Output.Logf(TEXT("\r\n%s{\r\n"), FCString::Tab(Indent));

	// declare and zero-initialize the parameters and return value, if applicable
	if (!Parameters.HasParms())
		return;

	FString EventStructName = GetEventStructParamsName(FunctionOuter, FunctionName);

	Output.Logf(TEXT("%s%s Parms;\r\n"), FCString::Tab(Indent + 1), *EventStructName );

	// Declare a parameter struct for this event/delegate and assign the struct members using the values passed into the event/delegate call.
	for (auto It = Parameters.Parms.CreateConstIterator(); It; ++It)
	{
		UProperty* Prop = *It;

		const FString PropertyName = Prop->GetName();
		if (Prop->ArrayDim > 1)
		{
			Output.Logf(TEXT("%sFMemory::Memcpy(Parms.%s,%s,sizeof(Parms.%s));\r\n"), FCString::Tab(Indent + 1), *PropertyName, *PropertyName, *PropertyName);
		}
		else
		{
			FString ValueAssignmentText = PropertyName;
			if (Prop->IsA<UBoolProperty>())
			{
				ValueAssignmentText += TEXT(" ? true : false");
			}

			Output.Logf(TEXT("%sParms.%s=%s;\r\n"), FCString::Tab(Indent + 1), *PropertyName, *ValueAssignmentText);
		}
	}
}

void WriteEventFunctionEpilogue(FOutputDevice& Output, int32 Indent, const FParmsAndReturnProperties& Parameters, const TCHAR* FunctionName)
{
	// Out parm copying.
	for (auto It = Parameters.Parms.CreateConstIterator(); It; ++It)
	{
		UProperty* Prop = *It;

		if (Prop->HasAnyPropertyFlags(CPF_OutParm) && (!Prop->HasAnyPropertyFlags(CPF_ConstParm) || Prop->IsA<UObjectPropertyBase>()))
		{
			const FString PropertyName = Prop->GetName();
			if ( Prop->ArrayDim > 1 )
			{
				Output.Logf(TEXT("%sFMemory::Memcpy(&%s,&Parms.%s,sizeof(%s));\r\n"), FCString::Tab(Indent + 1), *PropertyName, *PropertyName, *PropertyName);
			}
			else
			{
				Output.Logf(TEXT("%s%s=Parms.%s;\r\n"), FCString::Tab(Indent + 1), *PropertyName, *PropertyName);
			}
		}
	}

	// Return value.
	if (Parameters.Return)
	{
		// Make sure uint32 -> bool is supported
		bool bBoolProperty = Parameters.Return->IsA(UBoolProperty::StaticClass());
		Output.Logf(TEXT("%sreturn %sParms.%s;\r\n"), FCString::Tab(Indent + 1), bBoolProperty ? TEXT("!!") : TEXT(""), *Parameters.Return->GetName());
	}
	Output.Logf(TEXT("%s}\r\n"), FCString::Tab(Indent));
}

void FNativeClassHeaderGenerator::ExportDelegateDeclaration(FOutputDevice& Out, FOutputDevice& OutDeclarations, const FUnrealSourceFile& SourceFile, UFunction* Function)
{
	static const TCHAR DelegateStr[] = TEXT("delegate");

	check(Function->HasAnyFunctionFlags(FUNC_Delegate));

	const bool bIsMulticastDelegate = Function->HasAnyFunctionFlags( FUNC_MulticastDelegate );

	// Unmangle the function name
	const FString DelegateName = Function->GetName().LeftChop( FString( HEADER_GENERATED_DELEGATE_SIGNATURE_SUFFIX ).Len() );

	const FFunctionData* CompilerInfo = FFunctionData::FindForFunction(Function);

	FFuncInfo FunctionData = CompilerInfo->GetFunctionData();

	// Add class name to beginning of function, to avoid collisions with other classes with the same delegate name in this scope
	check(FunctionData.MarshallAndCallName.StartsWith(DelegateStr));
	FString ShortName = *FunctionData.MarshallAndCallName + ARRAY_COUNT(DelegateStr) - 1;
	FunctionData.MarshallAndCallName = FString::Printf( TEXT( "F%s_DelegateWrapper" ), *ShortName );

	// Setup delegate parameter
	const FString ExtraParam = FString::Printf(
		TEXT( "const %s& %s" ),
		bIsMulticastDelegate ? TEXT( "FMulticastScriptDelegate" ) : TEXT( "FScriptDelegate" ),
		*DelegateName
	);

	FUHTStringBuilder DelegateOutput;
	DelegateOutput.Log(TEXT("static "));

	// export the line that looks like: int32 Main(const FString& Parms)
	ExportNativeFunctionHeader(DelegateOutput, ForwardDeclarations, FunctionData, EExportFunctionType::Event, EExportFunctionHeaderStyle::Declaration, *ExtraParam, *GetAPIString());

	// Only exporting function prototype
	DelegateOutput.Logf(TEXT(";\r\n"));

	OutDeclarations.Log(FTypeSingletonCache::Get(Function).GetExternDecl());
	ExportFunction(Out, SourceFile, Function, false);
}

void FNativeClassHeaderGenerator::ExportDelegateDefinition(FOutputDevice& Out, const FUnrealSourceFile& SourceFile, UFunction* Function)
{
	static const TCHAR DelegateStr[] = TEXT("delegate");

	check(Function->HasAnyFunctionFlags(FUNC_Delegate));

	// Export parameters structs for all delegates.  We'll need these to declare our delegate execution function.
	FUHTStringBuilder DelegateOutput;
	ExportEventParm(DelegateOutput, ForwardDeclarations, Function, /*Indent=*/ 0, /*bOutputConstructor=*/ true, EExportingState::Normal);

	const bool bIsMulticastDelegate = Function->HasAnyFunctionFlags( FUNC_MulticastDelegate );

	// Unmangle the function name
	const FString DelegateName = Function->GetName().LeftChop( FString( HEADER_GENERATED_DELEGATE_SIGNATURE_SUFFIX ).Len() );

	const FFunctionData* CompilerInfo = FFunctionData::FindForFunction(Function);

	FFuncInfo FunctionData = CompilerInfo->GetFunctionData();

	// Always export delegate wrapper functions as inline
	FunctionData.FunctionExportFlags |= FUNCEXPORT_Inline;

	// Add class name to beginning of function, to avoid collisions with other classes with the same delegate name in this scope
	check(FunctionData.MarshallAndCallName.StartsWith(DelegateStr));
	FString ShortName = *FunctionData.MarshallAndCallName + ARRAY_COUNT(DelegateStr) - 1;
	FunctionData.MarshallAndCallName = FString::Printf( TEXT( "F%s_DelegateWrapper" ), *ShortName );

	// Setup delegate parameter
	const FString ExtraParam = FString::Printf(
		TEXT( "const %s& %s" ),
		bIsMulticastDelegate ? TEXT( "FMulticastScriptDelegate" ) : TEXT( "FScriptDelegate" ),
		*DelegateName
	);

	DelegateOutput.Log(TEXT("static "));

	// export the line that looks like: int32 Main(const FString& Parms)
	ExportNativeFunctionHeader(DelegateOutput, ForwardDeclarations, FunctionData, EExportFunctionType::Event, EExportFunctionHeaderStyle::Declaration, *ExtraParam, *GetAPIString());

	FParmsAndReturnProperties Parameters = GetFunctionParmsAndReturn(FunctionData.FunctionReference);

	WriteEventFunctionPrologue(DelegateOutput, 0, Parameters, Function->GetOuter(), *DelegateName);
	{
		const TCHAR* DelegateType = bIsMulticastDelegate ? TEXT( "ProcessMulticastDelegate" ) : TEXT( "ProcessDelegate" );
		const TCHAR* DelegateArg  = Parameters.HasParms() ? TEXT("&Parms") : TEXT("NULL");
		DelegateOutput.Logf(TEXT("\t%s.%s<UObject>(%s);\r\n"), *DelegateName, DelegateType, DelegateArg);
	}
	WriteEventFunctionEpilogue(DelegateOutput, 0, Parameters, *DelegateName);

	FString MacroName = SourceFile.GetGeneratedMacroName(FunctionData.MacroLine, TEXT("_DELEGATE"));
	WriteMacro(Out, MacroName, DelegateOutput);
}

void FNativeClassHeaderGenerator::ExportEventParm(FUHTStringBuilder& Out, TSet<FString>& PropertyFwd, UFunction* Function, int32 Indent, bool bOutputConstructor, EExportingState ExportingState)
{
	if (!WillExportEventParms(Function))
	{
		return;
	}

	FString FunctionName = Function->GetName();
	if (Function->HasAnyFunctionFlags(FUNC_Delegate))
	{
		FunctionName = FunctionName.LeftChop(FString(HEADER_GENERATED_DELEGATE_SIGNATURE_SUFFIX).Len());
	}

	FString EventParmStructName = GetEventStructParamsName(Function->GetOuter(), *FunctionName);
	Out.Logf(TEXT("%sstruct %s\r\n"), FCString::Tab(Indent), *EventParmStructName);
	Out.Logf(TEXT("%s{\r\n"), FCString::Tab(Indent));

	for (UProperty* Prop : TFieldRange<UProperty>(Function))
	{
		if (!(Prop->PropertyFlags & CPF_Parm))
		{
			continue;
		}

		PropertyFwd.Add(Prop->GetCPPTypeForwardDeclaration());

		FUHTStringBuilder PropertyText;
		PropertyText.Log(FCString::Tab(Indent + 1));

		bool bEmitConst = Prop->HasAnyPropertyFlags(CPF_ConstParm) && Prop->IsA<UObjectProperty>();

		//@TODO: UCREMOVAL: This is awful code duplication to avoid a double-const
		{
			// export 'const' for parameters
			const bool bIsConstParam = (Prop->IsA(UInterfaceProperty::StaticClass()) && !Prop->HasAllPropertyFlags(CPF_OutParm)); //@TODO: This should be const once that flag exists
			const bool bIsOnConstClass = (Prop->IsA(UObjectProperty::StaticClass()) && ((UObjectProperty*)Prop)->PropertyClass != NULL && ((UObjectProperty*)Prop)->PropertyClass->HasAnyClassFlags(CLASS_Const));

			if (bIsConstParam || bIsOnConstClass)
			{
				bEmitConst = false; // ExportCppDeclaration will do it for us
			}
		}

		if (bEmitConst)
		{
			PropertyText.Logf(TEXT("const "));
		}

		const FString* Dim = GArrayDimensions.Find(Prop);
		Prop->ExportCppDeclaration(PropertyText, EExportedDeclaration::Local, Dim ? **Dim : NULL);
		ApplyAlternatePropertyExportText(Prop, PropertyText, ExportingState);

		PropertyText.Log(TEXT(";\r\n"));
		Out += *PropertyText;

	}
	// constructor must initialize the return property if it needs it
	UProperty* Prop = Function->GetReturnProperty();
	if (Prop && bOutputConstructor)
	{
		FUHTStringBuilder InitializationAr;

		UStructProperty* InnerStruct = Cast<UStructProperty>(Prop);
		bool bNeedsOutput = true;
		if (InnerStruct)
		{
			bNeedsOutput = InnerStruct->HasNoOpConstructor();
		}
		else if (
			Cast<UNameProperty>(Prop) ||
			Cast<UDelegateProperty>(Prop) ||
			Cast<UMulticastDelegateProperty>(Prop) ||
			Cast<UStrProperty>(Prop) ||
			Cast<UTextProperty>(Prop) ||
			Cast<UArrayProperty>(Prop) ||
			Cast<UMapProperty>(Prop) ||
			Cast<USetProperty>(Prop) ||
			Cast<UInterfaceProperty>(Prop)
			)
		{
			bNeedsOutput = false;
		}
		if (bNeedsOutput)
		{
			check(Prop->ArrayDim == 1); // can't return arrays
			Out.Logf(TEXT("\r\n%s/** Constructor, initializes return property only **/\r\n"), FCString::Tab(Indent + 1));
			Out.Logf(TEXT("%s%s()\r\n"), FCString::Tab(Indent + 1), *EventParmStructName);
			Out.Logf(TEXT("%s%s %s(%s)\r\n"), FCString::Tab(Indent + 2), TEXT(":"), *Prop->GetName(), *GetNullParameterValue(Prop, false, true));
			Out.Logf(TEXT("%s{\r\n"), FCString::Tab(Indent + 1));
			Out.Logf(TEXT("%s}\r\n"), FCString::Tab(Indent + 1));
		}
	}
	Out.Logf(TEXT("%s};\r\n"), FCString::Tab(Indent));
}

/**
 * Get the intrinsic null value for this property
 * 
 * @param	Prop				the property to get the null value for
 * @param	bMacroContext		true when exporting the P_GET* macro, false when exporting the friendly C++ function header
 *
 * @return	the intrinsic null value for the property (0 for ints, TEXT("") for strings, etc.)
 */
FString FNativeClassHeaderGenerator::GetNullParameterValue( UProperty* Prop, bool bMacroContext, bool bInitializer/*=false*/ )
{
	UClass* PropClass = Prop->GetClass();
	UObjectPropertyBase* ObjectProperty = Cast<UObjectPropertyBase>(Prop);
	if (PropClass == UByteProperty::StaticClass())
	{
		UByteProperty* ByteProp = (UByteProperty*)Prop;

		// if it's an enum class then we need an explicit cast
		if( ByteProp->Enum && ByteProp->Enum->GetCppForm() == UEnum::ECppForm::EnumClass )
		{
			return FString::Printf(TEXT("(%s)0"), *ByteProp->GetCPPType());
		}

		return TEXT("0");
	}
	else if (PropClass == UEnumProperty::StaticClass())
	{
		UEnumProperty* EnumProp = (UEnumProperty*)Prop;

		return FString::Printf(TEXT("(%s)0"), *EnumProp->Enum->GetName());
	}
	else if ( PropClass == UBoolProperty::StaticClass() )
	{
		return TEXT("false");
	}
	else if ( PropClass == UIntProperty::StaticClass()
	||	PropClass == UFloatProperty::StaticClass()
	||	PropClass == UDoubleProperty::StaticClass())
	{
		return TEXT("0");
	}
	else if ( PropClass == UNameProperty::StaticClass() )
	{
		return TEXT("NAME_None");
	}
	else if ( PropClass == UStrProperty::StaticClass() )
	{
		return TEXT("TEXT(\"\")");
	}
	else if ( PropClass == UTextProperty::StaticClass() )
	{
		return TEXT("FText::GetEmpty()");
	}
	else if ( PropClass == UArrayProperty::StaticClass()
		||    PropClass == UMapProperty::StaticClass()
		||    PropClass == USetProperty::StaticClass()
		||    PropClass == UDelegateProperty::StaticClass()
		||    PropClass == UMulticastDelegateProperty::StaticClass() )
	{
		FString Type, ExtendedType;
		Type = Prop->GetCPPType(&ExtendedType,CPPF_OptionalValue);
		return Type + ExtendedType + TEXT("()");
	}
	else if ( PropClass == UStructProperty::StaticClass() )
	{
		bool bHasNoOpConstuctor = CastChecked<UStructProperty>(Prop)->HasNoOpConstructor();
		if (bInitializer && bHasNoOpConstuctor)
		{
			return TEXT("ForceInit");
		}

		FString Type, ExtendedType;
		Type = Prop->GetCPPType(&ExtendedType,CPPF_OptionalValue);
		return Type + ExtendedType + (bHasNoOpConstuctor ? TEXT("(ForceInit)") : TEXT("()"));
	}
	else if (ObjectProperty)
	{
		return TEXT("NULL");
	}
	else if ( PropClass == UInterfaceProperty::StaticClass() )
	{
		return TEXT("NULL");
	}

	UE_LOG(LogCompile, Fatal,TEXT("GetNullParameterValue - Unhandled property type '%s': %s"), *PropClass->GetName(), *Prop->GetPathName());
	return TEXT("");
}


FString FNativeClassHeaderGenerator::GetFunctionReturnString(UFunction* Function)
{
	if (UProperty* Return = Function->GetReturnProperty())
	{
		FString ExtendedReturnType;
		ForwardDeclarations.Add(Return->GetCPPTypeForwardDeclaration());
		FString ReturnType = Return->GetCPPType(&ExtendedReturnType, CPPF_ArgumentOrReturnValue);
		FUHTStringBuilder ReplacementText;
		ReplacementText += ReturnType;
		ApplyAlternatePropertyExportText(Return, ReplacementText, EExportingState::Normal);
		return ReplacementText + ExtendedReturnType;
	}

	return TEXT("void");
}

/**
* Gets string with function const modifier type.
*
* @param Function Function to get const modifier of.
* @return Empty FString if function is non-const, FString("const") if function is const.
*/
FString GetFunctionConstModifierString(UFunction* Function)
{
	if (Function->HasAllFunctionFlags(FUNC_Const))
	{
		return TEXT("const");
	}

	return FString();
}

/**
 * Converts Position within File to Line and Column.
 *
 * @param File File contents.
 * @param Position Position in string to convert.
 * @param OutLine Result line.
 * @param OutColumn Result column.
 */
void GetLineAndColumnFromPositionInFile(const FString& File, int32 Position, int32& OutLine, int32& OutColumn)
{
	OutLine = 1;
	OutColumn = 1;

	int32 i;
	for (i = 1; i <= Position; ++i)
	{
		if (File[i] == '\n')
		{
			++OutLine;
			OutColumn = 0;
		}
		else
		{
			++OutColumn;
		}
	}
}

bool FNativeClassHeaderGenerator::IsMissingVirtualSpecifier(const FString& SourceFile, int32 FunctionNamePosition)
{
	auto IsEndOfSearchChar = [](TCHAR C) { return (C == TEXT('}')) || (C == TEXT('{')) || (C == TEXT(';')); };

	// Find first occurrence of "}", ";", "{" going backwards from ImplementationPosition.
	int32 EndOfSearchCharIndex = SourceFile.FindLastCharByPredicate(IsEndOfSearchChar, FunctionNamePosition);
	check(EndOfSearchCharIndex != INDEX_NONE);

	// Then find if there is "virtual" keyword starting from position of found character to ImplementationPosition
	return !HasIdentifierExactMatch(&SourceFile[EndOfSearchCharIndex], &SourceFile[FunctionNamePosition], TEXT("virtual"));
}

FString CreateClickableErrorMessage(const FString& Filename, int32 Line, int32 Column)
{
	return FString::Printf(TEXT("%s(%d,%d): error: "), *Filename, Line, Column);
}

void FNativeClassHeaderGenerator::CheckRPCFunctions(const FFuncInfo& FunctionData, const FString& ClassName, int32 ImplementationPosition, int32 ValidatePosition, const FUnrealSourceFile& SourceFile)
{
	bool bHasImplementation = ImplementationPosition != INDEX_NONE;
	bool bHasValidate = ValidatePosition != INDEX_NONE;

	auto Function = FunctionData.FunctionReference;
	auto FunctionReturnType = GetFunctionReturnString(Function);
	auto ConstModifier = GetFunctionConstModifierString(Function) + TEXT(" ");

	auto bIsNative = Function->HasAllFunctionFlags(FUNC_Native);
	auto bIsNet = Function->HasAllFunctionFlags(FUNC_Net);
	auto bIsNetValidate = Function->HasAllFunctionFlags(FUNC_NetValidate);
	auto bIsNetResponse = Function->HasAllFunctionFlags(FUNC_NetResponse);
	auto bIsBlueprintEvent = Function->HasAllFunctionFlags(FUNC_BlueprintEvent);

	bool bNeedsImplementation = (bIsNet && !bIsNetResponse) || bIsBlueprintEvent || bIsNative;
	bool bNeedsValidate = (bIsNative || bIsNet) && !bIsNetResponse && bIsNetValidate;

	check(bNeedsImplementation || bNeedsValidate);

	auto ParameterString = GetFunctionParameterString(Function);
	const auto& Filename = SourceFile.GetFilename();
	const auto& FileContent = SourceFile.GetContent();

	//
	// Get string with function specifiers, listing why we need _Implementation or _Validate functions.
	//
	TArray<FString> FunctionSpecifiers;
	FunctionSpecifiers.Reserve(4);
	if (bIsNative)			{ FunctionSpecifiers.Add(TEXT("Native"));			}
	if (bIsNet)				{ FunctionSpecifiers.Add(TEXT("Net"));				}
	if (bIsBlueprintEvent)	{ FunctionSpecifiers.Add(TEXT("BlueprintEvent"));	}
	if (bIsNetValidate)		{ FunctionSpecifiers.Add(TEXT("NetValidate"));		}

	check(FunctionSpecifiers.Num() > 0);

	//
	// Coin static_assert message
	//
	FUHTStringBuilder AssertMessage;
	AssertMessage.Logf(TEXT("Function %s was marked as %s"), *(Function->GetName()), *FunctionSpecifiers[0]);
	for (int32 i = 1; i < FunctionSpecifiers.Num(); ++i)
	{
		AssertMessage.Logf(TEXT(", %s"), *FunctionSpecifiers[i]);
	}

	AssertMessage.Logf(TEXT("."));

	//
	// Check if functions are missing.
	//
	int32 Line;
	int32 Column;
	GetLineAndColumnFromPositionInFile(FileContent, FunctionData.InputPos, Line, Column);
	if (bNeedsImplementation && !bHasImplementation)
	{
		FString ErrorPosition = CreateClickableErrorMessage(Filename, Line, Column);
		FString FunctionDecl = FString::Printf(TEXT("virtual %s %s::%s(%s) %s"), *FunctionReturnType, *ClassName, *FunctionData.CppImplName, *ParameterString, *ConstModifier);
		FError::Throwf(TEXT("%s%s Declare function %s"), *ErrorPosition, *AssertMessage, *FunctionDecl);
	}
	
	if (bNeedsValidate && !bHasValidate)
	{
		FString ErrorPosition = CreateClickableErrorMessage(Filename, Line, Column);
		FString FunctionDecl = FString::Printf(TEXT("virtual bool %s::%s(%s) %s"), *ClassName, *FunctionData.CppValidationImplName, *ParameterString, *ConstModifier);
		FError::Throwf(TEXT("%s%s Declare function %s"), *ErrorPosition, *AssertMessage, *FunctionDecl);
	}

	//
	// If all needed functions are declared, check if they have virtual specifiers.
	//
	if (bNeedsImplementation && bHasImplementation && IsMissingVirtualSpecifier(FileContent, ImplementationPosition))
	{
		GetLineAndColumnFromPositionInFile(FileContent, ImplementationPosition, Line, Column);
		FString ErrorPosition = CreateClickableErrorMessage(Filename, Line, Column);
		FString FunctionDecl = FString::Printf(TEXT("%s %s::%s(%s) %s"), *FunctionReturnType, *ClassName, *FunctionData.CppImplName, *ParameterString, *ConstModifier);
		FError::Throwf(TEXT("%sDeclared function %sis not marked as virtual."), *ErrorPosition, *FunctionDecl);
	}

	if (bNeedsValidate && bHasValidate && IsMissingVirtualSpecifier(FileContent, ValidatePosition))
	{
		GetLineAndColumnFromPositionInFile(FileContent, ValidatePosition, Line, Column);
		FString ErrorPosition = CreateClickableErrorMessage(Filename, Line, Column);
		FString FunctionDecl = FString::Printf(TEXT("bool %s::%s(%s) %s"), *ClassName, *FunctionData.CppValidationImplName, *ParameterString, *ConstModifier);
		FError::Throwf(TEXT("%sDeclared function %sis not marked as virtual."), *ErrorPosition, *FunctionDecl);
	}
}

void FNativeClassHeaderGenerator::ExportNativeFunctionHeader(
	FOutputDevice&                   Out,
	TSet<FString>&                   OutFwdDecls,
	const FFuncInfo&                 FunctionData,
	EExportFunctionType::Type        FunctionType,
	EExportFunctionHeaderStyle::Type FunctionHeaderStyle,
	const TCHAR*                     ExtraParam,
	const TCHAR*                     APIString
)
{
	UFunction* Function = FunctionData.FunctionReference;

	const bool bIsDelegate   = Function->HasAnyFunctionFlags( FUNC_Delegate );
	const bool bIsInterface  = !bIsDelegate && Function->GetOwnerClass()->HasAnyClassFlags(CLASS_Interface);
	const bool bIsK2Override = Function->HasAnyFunctionFlags( FUNC_BlueprintEvent );
	
	if (!bIsDelegate)
	{
		Out.Log(TEXT("\t"));
	}

	if (FunctionHeaderStyle == EExportFunctionHeaderStyle::Declaration)
	{
		// cpp implementation of functions never have these appendages

		// If the function was marked as 'RequiredAPI', then add the *_API macro prefix.  Note that if the class itself
		// was marked 'RequiredAPI', this is not needed as C++ will exports all methods automatically.
		if (FunctionType != EExportFunctionType::Event &&
			!Function->GetOwnerClass()->HasAnyClassFlags(CLASS_RequiredAPI) &&
			(FunctionData.FunctionExportFlags & FUNCEXPORT_RequiredAPI))
		{
			Out.Log(APIString);
		}

		if(FunctionType == EExportFunctionType::Interface)
		{
			Out.Log(TEXT("static "));
		}
		else if (bIsK2Override)
		{
			Out.Log(TEXT("virtual "));
		}
		// if the owning class is an interface class
		else if ( bIsInterface )
		{
			Out.Log(TEXT("virtual "));
		}
		// this is not an event, the function is not a static function and the function is not marked final
		else if ( FunctionType != EExportFunctionType::Event && !Function->HasAnyFunctionFlags(FUNC_Static) && !(FunctionData.FunctionExportFlags & FUNCEXPORT_Final) )
		{
			Out.Log(TEXT("virtual "));
		}
		else if( FunctionData.FunctionExportFlags & FUNCEXPORT_Inline )
		{
			Out.Log(TEXT("inline "));
		}
	}

	if (UProperty* Return = Function->GetReturnProperty())
	{
		FString ExtendedReturnType;
		FString ReturnType = Return->GetCPPType(&ExtendedReturnType, (FunctionHeaderStyle == EExportFunctionHeaderStyle::Definition && (FunctionType != EExportFunctionType::Interface) ? CPPF_Implementation : 0) | CPPF_ArgumentOrReturnValue);
		OutFwdDecls.Add(Return->GetCPPTypeForwardDeclaration());
		FUHTStringBuilder ReplacementText;
		ReplacementText += ReturnType;
		ApplyAlternatePropertyExportText(Return, ReplacementText, EExportingState::Normal);
		Out.Logf(TEXT("%s%s"), *ReplacementText, *ExtendedReturnType);
	}
	else
	{
		Out.Log( TEXT("void") );
	}

	FString FunctionName;
	if (FunctionHeaderStyle == EExportFunctionHeaderStyle::Definition)
	{
		FunctionName = FString(NameLookupCPP.GetNameCPP(CastChecked<UClass>(Function->GetOuter()), bIsInterface || FunctionType == EExportFunctionType::Interface)) + TEXT("::");
	}

	if (FunctionType == EExportFunctionType::Interface)
	{
		FunctionName += FString::Printf(TEXT("Execute_%s"), *Function->GetName());
	}
	else if (FunctionType == EExportFunctionType::Event)
	{
		FunctionName += FunctionData.MarshallAndCallName;
	}
	else
	{
		FunctionName += FunctionData.CppImplName;
	}

	Out.Logf(TEXT(" %s("), *FunctionName);

	int32 ParmCount=0;

	// Emit extra parameter if we have one
	if( ExtraParam )
	{
		Out.Logf(TEXT("%s"), ExtraParam);
		++ParmCount;
	}

	for (UProperty* Property : TFieldRange<UProperty>(Function))
	{
		if ((Property->PropertyFlags & (CPF_Parm | CPF_ReturnParm)) != CPF_Parm)
		{
			continue;
		}

		OutFwdDecls.Add(Property->GetCPPTypeForwardDeclaration());

		if( ParmCount++ )
		{
			Out.Log(TEXT(", "));
		}

		FUHTStringBuilder PropertyText;

		const FString* Dim = GArrayDimensions.Find(Property);
		Property->ExportCppDeclaration( PropertyText, EExportedDeclaration::Parameter, Dim ? **Dim : NULL );
		ApplyAlternatePropertyExportText(Property, PropertyText, EExportingState::Normal);

		Out.Logf(TEXT("%s"), *PropertyText);
	}

	Out.Log( TEXT(")") );
	if (FunctionType != EExportFunctionType::Interface)
	{
		if (!bIsDelegate && Function->HasAllFunctionFlags(FUNC_Const))
		{
			Out.Log( TEXT(" const") );
		}

		if (bIsInterface && FunctionHeaderStyle == EExportFunctionHeaderStyle::Declaration)
		{
			// all methods in interface classes are pure virtuals
			Out.Log(TEXT("=0"));
		}
	}
}

/**
 * Export the actual internals to a standard thunk function
 *
 * @param RPCWrappers output device for writing
 * @param FunctionData function data for the current function
 * @param Parameters list of parameters in the function
 * @param Return return parameter for the function
 * @param DeprecationWarningOutputDevice Device to output deprecation warnings for _Validate and _Implementation functions.
 */
void FNativeClassHeaderGenerator::ExportFunctionThunk(FUHTStringBuilder& RPCWrappers, UFunction* Function, const FFuncInfo& FunctionData, const TArray<UProperty*>& Parameters, UProperty* Return, FUHTStringBuilder& DeprecationWarningOutputDevice)
{
	// export the GET macro for this parameter
	FString ParameterList;
	for (int32 ParameterIndex = 0; ParameterIndex < Parameters.Num(); ParameterIndex++)
	{
		UProperty* Param = Parameters[ParameterIndex];
		ForwardDeclarations.Add(Param->GetCPPTypeForwardDeclaration());

		FString EvalBaseText = TEXT("P_GET_");	// e.g. P_GET_STR
		FString EvalModifierText;				// e.g. _REF
		FString EvalParameterText;				// e.g. (UObject*,NULL)

		FString TypeText;

		if (Param->ArrayDim > 1)
		{
			EvalBaseText += TEXT("ARRAY");
			TypeText = Param->GetCPPType();
		}
		else
		{
			EvalBaseText += Param->GetCPPMacroType(TypeText);

			UArrayProperty* ArrayProperty = Cast<UArrayProperty>(Param);
			if (ArrayProperty)
			{
				UInterfaceProperty* InterfaceProperty = Cast<UInterfaceProperty>(ArrayProperty->Inner);
				if (InterfaceProperty)
				{
					FString InterfaceTypeText;
					InterfaceProperty->GetCPPMacroType(InterfaceTypeText);
					TypeText += FString::Printf(TEXT("<%s>"), *InterfaceTypeText);
				}
			}
		}

		bool bPassAsNoPtr = Param->HasAllPropertyFlags(CPF_UObjectWrapper | CPF_OutParm) && Param->IsA(UClassProperty::StaticClass());
		if (bPassAsNoPtr)
		{
			TypeText = Param->GetCPPType();
		}

		FUHTStringBuilder ReplacementText;
		ReplacementText += TypeText;

		ApplyAlternatePropertyExportText(Param, ReplacementText, EExportingState::Normal);
		TypeText = ReplacementText;

		FString DefaultValueText;
		FString ParamPrefix = TEXT("Z_Param_");

		// if this property is an out parm, add the REF tag
		if (Param->PropertyFlags & CPF_OutParm)
		{
			if (!bPassAsNoPtr)
			{
				EvalModifierText += TEXT("_REF");
			}
			else
			{
				// Parameters passed as TSubclassOf<Class>& shouldn't have asterisk added.
				EvalModifierText += TEXT("_REF_NO_PTR");	
			}

			ParamPrefix += TEXT("Out_");
		}

		// if this property requires a specialization, add a comma to the type name so we can print it out easily
		if (TypeText != TEXT(""))
		{
			TypeText += TCHAR(',');
		}

		FString ParamName = ParamPrefix + Param->GetName();

		EvalParameterText = FString::Printf(TEXT("(%s%s%s)"), *TypeText, *ParamName, *DefaultValueText);

		RPCWrappers.Logf(TEXT("\t\t%s%s%s;") LINE_TERMINATOR, *EvalBaseText, *EvalModifierText, *EvalParameterText);

		// add this property to the parameter list string
		if (ParameterList.Len())
		{
			ParameterList += TCHAR(',');
		}

		{
			UDelegateProperty* DelegateProp = Cast< UDelegateProperty >(Param);
			if (DelegateProp != NULL)
			{
				// For delegates, add an explicit conversion to the specific type of delegate before passing it along
				const FString FunctionName = DelegateProp->SignatureFunction->GetName().LeftChop(FString(HEADER_GENERATED_DELEGATE_SIGNATURE_SUFFIX).Len());
				const FString CPPDelegateName = FString(TEXT("F")) + FunctionName;
				ParamName = FString::Printf(TEXT("%s(%s)"), *CPPDelegateName, *ParamName);
			}
		}

		{
			UMulticastDelegateProperty* MulticastDelegateProp = Cast< UMulticastDelegateProperty >(Param);
			if (MulticastDelegateProp != NULL)
			{
				// For delegates, add an explicit conversion to the specific type of delegate before passing it along
				const FString FunctionName = MulticastDelegateProp->SignatureFunction->GetName().LeftChop(FString(HEADER_GENERATED_DELEGATE_SIGNATURE_SUFFIX).Len());
				const FString CPPDelegateName = FString(TEXT("F")) + FunctionName;
				ParamName = FString::Printf(TEXT("%s(%s)"), *CPPDelegateName, *ParamName);
			}
		}

		UEnum* Enum = nullptr;
		UByteProperty* ByteProp = Cast<UByteProperty>(Param);
		if (ByteProp && ByteProp->Enum)
		{
			Enum = ByteProp->Enum;
		}
		else if (Param->IsA<UEnumProperty>())
		{
			Enum = ((UEnumProperty*)Param)->Enum;
		}

		if (Enum)
		{
			// For enums, add an explicit conversion 
			if (!(Param->PropertyFlags & CPF_OutParm))
			{
				ParamName = FString::Printf(TEXT("%s(%s)"), *Enum->CppType, *ParamName);
			}
			else
			{
				if (Enum->GetCppForm() == UEnum::ECppForm::EnumClass)
				{
					// If we're an enum class don't require the wrapper
					ParamName = FString::Printf(TEXT("(%s&)(%s)"), *Enum->CppType, *ParamName);
				}
				else
				{
					ParamName = FString::Printf(TEXT("(TEnumAsByte<%s>&)(%s)"), *Enum->CppType, *ParamName);
				}
			}
		}

		ParameterList += ParamName;
	}

	RPCWrappers += TEXT("\t\tP_FINISH;") LINE_TERMINATOR;
	RPCWrappers += TEXT("\t\tP_NATIVE_BEGIN;") LINE_TERMINATOR;

	ClassDefinitionRange ClassRange;
	if (ClassDefinitionRanges.Contains(Function->GetOwnerClass()))
	{
		ClassRange = ClassDefinitionRanges[Function->GetOwnerClass()];
		ClassRange.Validate();
	}

	const TCHAR* ClassStart = ClassRange.Start;
	const TCHAR* ClassEnd   = ClassRange.End;
	FString      ClassName  = Function->GetOwnerClass()->GetName();

	FString ClassDefinition(ClassEnd - ClassStart, ClassStart);

	bool bHasImplementation = HasIdentifierExactMatch(ClassDefinition, FunctionData.CppImplName);
	bool bHasValidate = HasIdentifierExactMatch(ClassDefinition, FunctionData.CppValidationImplName);

	bool bShouldEnableImplementationDeprecation =
		// Enable deprecation warnings only if GENERATED_BODY is used inside class or interface (not GENERATED_UCLASS_BODY etc.)
		ClassRange.bHasGeneratedBody
		// and implementation function is called, but not the one declared by user
		&& (FunctionData.CppImplName != Function->GetName() && !bHasImplementation);

	bool bShouldEnableValidateDeprecation =
		// Enable deprecation warnings only if GENERATED_BODY is used inside class or interface (not GENERATED_UCLASS_BODY etc.)
		ClassRange.bHasGeneratedBody
		// and validation function is called
		&& (FunctionData.FunctionFlags & FUNC_NetValidate) && !bHasValidate;

	//Emit warning here if necessary
	FUHTStringBuilder FunctionDeclaration;
	ExportNativeFunctionHeader(FunctionDeclaration, ForwardDeclarations, FunctionData, EExportFunctionType::Function, EExportFunctionHeaderStyle::Declaration, nullptr, *GetAPIString());
	FunctionDeclaration.Trim();

	// Call the validate function if there is one
	if (!(FunctionData.FunctionExportFlags & FUNCEXPORT_CppStatic) && (FunctionData.FunctionFlags & FUNC_NetValidate))
	{
		RPCWrappers.Logf(TEXT("\t\tif (!this->%s(%s))") LINE_TERMINATOR, *FunctionData.CppValidationImplName, *ParameterList);
		RPCWrappers.Logf(TEXT("\t\t{") LINE_TERMINATOR);
		RPCWrappers.Logf(TEXT("\t\t\tRPC_ValidateFailed(TEXT(\"%s\"));") LINE_TERMINATOR, *FunctionData.CppValidationImplName);
		RPCWrappers.Logf(TEXT("\t\t\treturn;") LINE_TERMINATOR);	// If we got here, the validation function check failed
		RPCWrappers.Logf(TEXT("\t\t}") LINE_TERMINATOR);
	}

	// write out the return value
	RPCWrappers.Log(TEXT("\t\t"));
	if (Return)
	{
		ForwardDeclarations.Add(Return->GetCPPTypeForwardDeclaration());

		FUHTStringBuilder ReplacementText;
		FString ReturnExtendedType;
		ReplacementText += Return->GetCPPType(&ReturnExtendedType);
		ApplyAlternatePropertyExportText(Return, ReplacementText, EExportingState::Normal);

		FString ReturnType = ReplacementText;
		RPCWrappers.Logf(TEXT("*(%s%s*)") TEXT(PREPROCESSOR_TO_STRING(RESULT_PARAM)) TEXT("="), *ReturnType, *ReturnExtendedType);
	}

	// export the call to the C++ version
	if (FunctionData.FunctionExportFlags & FUNCEXPORT_CppStatic)
	{
		RPCWrappers.Logf(TEXT("%s::%s(%s);") LINE_TERMINATOR, NameLookupCPP.GetNameCPP(Function->GetOwnerClass()), *FunctionData.CppImplName, *ParameterList);
	}
	else
	{
		RPCWrappers.Logf(TEXT("this->%s(%s);") LINE_TERMINATOR, *FunctionData.CppImplName, *ParameterList);
	}
	RPCWrappers += TEXT("\t\tP_NATIVE_END;") LINE_TERMINATOR;
}

FString FNativeClassHeaderGenerator::GetFunctionParameterString(UFunction* Function)
{
	FString ParameterList;
	FUHTStringBuilder PropertyText;

	for (UProperty* Property : TFieldRange<UProperty>(Function))
	{
		ForwardDeclarations.Add(Property->GetCPPTypeForwardDeclaration());

		if ((Property->PropertyFlags & (CPF_Parm | CPF_ReturnParm)) != CPF_Parm)
		{
			break;
		}

		if (ParameterList.Len())
		{
			ParameterList += TEXT(", ");
		}

		auto Dim = GArrayDimensions.Find(Property);
		Property->ExportCppDeclaration(PropertyText, EExportedDeclaration::Parameter, Dim ? **Dim : NULL, 0, true);
		ApplyAlternatePropertyExportText(Property, PropertyText, EExportingState::Normal);

		ParameterList += PropertyText;
		PropertyText.Reset();
	}

	return ParameterList;
}

void FNativeClassHeaderGenerator::ExportNativeFunctions(FOutputDevice& OutGeneratedHeaderText, FOutputDevice& OutMacroCalls, FOutputDevice& OutNoPureDeclsMacroCalls, const FUnrealSourceFile& SourceFile, UClass* Class, FClassMetaData* ClassData)
{
	FUHTStringBuilder RPCWrappers;
	FUHTStringBuilder AutogeneratedBlueprintFunctionDeclarations;
	FUHTStringBuilder AutogeneratedBlueprintFunctionDeclarationsOnlyNotDeclared;

	FString ClassName = Class->GetName();

	ClassDefinitionRange ClassRange;
	if (ClassDefinitionRanges.Contains(Class))
	{
		ClassRange = ClassDefinitionRanges[Class];
		ClassRange.Validate();
	}

	// export the C++ stubs
	
	for (UFunction* Function : TFieldRange<UFunction>(Class, EFieldIteratorFlags::ExcludeSuper))
	{
		if (!(Function->FunctionFlags & FUNC_Native))
		{
			continue;
		}

		FFunctionData* CompilerInfo = FFunctionData::FindForFunction(Function);

		const FFuncInfo& FunctionData = CompilerInfo->GetFunctionData();

		// Custom thunks don't get any C++ stub function generated
		if (FunctionData.FunctionExportFlags & FUNCEXPORT_CustomThunk)
		{
			continue;
		}

		// Should we emit these to RPC wrappers or just ignore them?
		const bool bWillBeProgrammerTyped = FunctionData.CppImplName == Function->GetName();

		if (!bWillBeProgrammerTyped)
		{
			const TCHAR* ClassStart = ClassRange.Start;
			const TCHAR* ClassEnd   = ClassRange.End;
			FString ClassDefinition(ClassEnd - ClassStart, ClassStart);

			FString FunctionName = Function->GetName();
			int32 ClassDefinitionStartPosition = ClassStart - *SourceFile.GetContent();

			int32 ImplementationPosition = FindIdentifierExactMatch(ClassDefinition, FunctionData.CppImplName);
			bool bHasImplementation = ImplementationPosition != INDEX_NONE;
			if (bHasImplementation)
			{
				ImplementationPosition += ClassDefinitionStartPosition;
			}

			int32 ValidatePosition = FindIdentifierExactMatch(ClassDefinition, FunctionData.CppValidationImplName);
			bool bHasValidate = ValidatePosition != INDEX_NONE;
			if (bHasValidate)
			{
				ValidatePosition += ClassDefinitionStartPosition;
			}

			//Emit warning here if necessary
			FUHTStringBuilder FunctionDeclaration;
			ExportNativeFunctionHeader(FunctionDeclaration, ForwardDeclarations, FunctionData, EExportFunctionType::Function, EExportFunctionHeaderStyle::Declaration, nullptr, *GetAPIString());
			FunctionDeclaration.Log(TEXT(";\r\n"));

			// Declare validation function if needed
			if (FunctionData.FunctionFlags & FUNC_NetValidate)
			{
				FString ParameterList = GetFunctionParameterString(Function);

				const TCHAR* Virtual = (!FunctionData.FunctionReference->HasAnyFunctionFlags(FUNC_Static) && !(FunctionData.FunctionExportFlags & FUNCEXPORT_Final)) ? TEXT("virtual") : TEXT("");
				FStringOutputDevice ValidDecl;
				ValidDecl.Logf(TEXT("\t%s bool %s(%s);\r\n"), Virtual, *FunctionData.CppValidationImplName, *ParameterList);
				AutogeneratedBlueprintFunctionDeclarations.Log(*ValidDecl);
				if (!bHasValidate)
				{
					AutogeneratedBlueprintFunctionDeclarationsOnlyNotDeclared.Logf(TEXT("%s"), *ValidDecl);
				}
			}

			AutogeneratedBlueprintFunctionDeclarations.Log(*FunctionDeclaration);
			if (!bHasImplementation && FunctionData.CppImplName != FunctionName)
			{
				AutogeneratedBlueprintFunctionDeclarationsOnlyNotDeclared.Log(*FunctionDeclaration);
			}

			// Versions that skip function autodeclaration throw an error when a function is missing.
			if (ClassRange.bHasGeneratedBody && (SourceFile.GetGeneratedCodeVersionForStruct(Class) > EGeneratedCodeVersion::V1))
			{
				FString Name = Class->HasAnyClassFlags(CLASS_Interface) ? TEXT("I") + ClassName : FString(NameLookupCPP.GetNameCPP(Class));
				CheckRPCFunctions(FunctionData, *Name, ImplementationPosition, ValidatePosition, SourceFile);
			}
		}

		RPCWrappers.Log(TEXT("\r\n"));

		// if this function was originally declared in a base class, and it isn't a static function,
		// only the C++ function header will be exported
		if (!ShouldExportUFunction(Function))
		{
			continue;
		}

		// export the script wrappers
		RPCWrappers.Logf(TEXT("\tDECLARE_FUNCTION(%s)"), *FunctionData.UnMarshallAndCallName);
		RPCWrappers += LINE_TERMINATOR TEXT("\t{") LINE_TERMINATOR;

		FParmsAndReturnProperties Parameters = GetFunctionParmsAndReturn(FunctionData.FunctionReference);
		ExportFunctionThunk(RPCWrappers, Function, FunctionData, Parameters.Parms, Parameters.Return, AutogeneratedBlueprintFunctionDeclarationsOnlyNotDeclared);

		RPCWrappers += TEXT("\t}") LINE_TERMINATOR;
	}

	FString MacroName = SourceFile.GetGeneratedMacroName(ClassData, TEXT("_RPC_WRAPPERS"));
	WriteMacro(OutGeneratedHeaderText, MacroName, AutogeneratedBlueprintFunctionDeclarations + RPCWrappers);
	OutMacroCalls.Logf(TEXT("\t%s\r\n"), *MacroName);

	// Put static checks before RPCWrappers to get proper messages from static asserts before compiler errors.
	FString NoPureDeclsMacroName = SourceFile.GetGeneratedMacroName(ClassData, TEXT("_RPC_WRAPPERS_NO_PURE_DECLS"));
	if (SourceFile.GetGeneratedCodeVersionForStruct(Class) > EGeneratedCodeVersion::V1)
	{
		WriteMacro(OutGeneratedHeaderText, NoPureDeclsMacroName, RPCWrappers);
	}
	else
	{
		WriteMacro(OutGeneratedHeaderText, NoPureDeclsMacroName, AutogeneratedBlueprintFunctionDeclarationsOnlyNotDeclared + RPCWrappers);
	}
	OutNoPureDeclsMacroCalls.Logf(TEXT("\t%s\r\n"), *NoPureDeclsMacroName);
}

/**
 * Exports the methods which trigger UnrealScript events and delegates.
 *
 * @param	CallbackFunctions	the functions to export
 */
void FNativeClassHeaderGenerator::ExportCallbackFunctions(
	FOutputDevice&            OutGeneratedHeaderText,
	FOutputDevice&            OutCpp,
	TSet<FString>&            OutFwdDecls,
	const TArray<UFunction*>& CallbackFunctions,
	const TCHAR*              CallbackWrappersMacroName,
	EExportCallbackType       ExportCallbackType,
	const TCHAR*              API,
	const TCHAR*              APIString
)
{
	FUHTStringBuilder RPCWrappers;
	for (UFunction* Function : CallbackFunctions)
	{
		// Never expecting to export delegate functions this way
		check(!Function->HasAnyFunctionFlags(FUNC_Delegate));

		FFunctionData*   CompilerInfo = FFunctionData::FindForFunction(Function);
		const FFuncInfo& FunctionData = CompilerInfo->GetFunctionData();
		FString          FunctionName = Function->GetName();
		UClass*          Class        = CastChecked<UClass>(Function->GetOuter());
		const TCHAR*     ClassName    = NameLookupCPP.GetNameCPP(Class);

		if (FunctionData.FunctionFlags & FUNC_NetResponse)
		{
			// Net response functions don't go into the VM
			continue;
		}

		const bool bWillBeProgrammerTyped = FunctionName == FunctionData.MarshallAndCallName;

		// Emit the declaration if the programmer isn't responsible for declaring this wrapper
		if (!bWillBeProgrammerTyped)
		{
			// export the line that looks like: int32 Main(const FString& Parms)
			ExportNativeFunctionHeader(RPCWrappers, OutFwdDecls, FunctionData, EExportFunctionType::Event, EExportFunctionHeaderStyle::Declaration, nullptr, APIString);

			RPCWrappers.Log(TEXT(";\r\n"));
			RPCWrappers.Log(TEXT("\r\n"));
		}

		FString FunctionNameName;
		if (ExportCallbackType != EExportCallbackType::Interface)
		{
			FunctionNameName = FString::Printf(TEXT("NAME_%s_%s"), ClassName, *FunctionName);
			OutCpp.Logf(TEXT("\tstatic FName %s = FName(TEXT(\"%s\"));") LINE_TERMINATOR, *FunctionNameName, *GetOverriddenFName(Function).ToString());
		}

		// Emit the thunk implementation
		ExportNativeFunctionHeader(OutCpp, OutFwdDecls, FunctionData, EExportFunctionType::Event, EExportFunctionHeaderStyle::Definition, nullptr, APIString);

		FParmsAndReturnProperties Parameters = GetFunctionParmsAndReturn(FunctionData.FunctionReference);

		if (ExportCallbackType != EExportCallbackType::Interface)
		{
			WriteEventFunctionPrologue(OutCpp, /*Indent=*/ 1, Parameters, Class, *FunctionName);
			{
				// Cast away const just in case, because ProcessEvent isn't const
				OutCpp.Logf(
					TEXT("\t\t%sProcessEvent(FindFunctionChecked(%s),%s);\r\n"),
					(Function->HasAllFunctionFlags(FUNC_Const)) ? *FString::Printf(TEXT("const_cast<%s*>(this)->"), ClassName) : TEXT(""),
					*FunctionNameName,
					Parameters.HasParms() ? TEXT("&Parms") : TEXT("NULL")
				);
			}
			WriteEventFunctionEpilogue(OutCpp, /*Indent=*/ 1, Parameters, *FunctionName);
		}
		else
		{
			OutCpp.Log(LINE_TERMINATOR);
			OutCpp.Log(TEXT("\t{") LINE_TERMINATOR);

			// assert if this is ever called directly
			OutCpp.Logf(TEXT("\t\tcheck(0 && \"Do not directly call Event functions in Interfaces. Call Execute_%s instead.\");") LINE_TERMINATOR, *FunctionName);

			// satisfy compiler if it's expecting a return value
			if (Parameters.Return)
			{
				FString EventParmStructName = GetEventStructParamsName(Class, *FunctionName);
				OutCpp.Logf(TEXT("\t\t%s Parms;") LINE_TERMINATOR, *EventParmStructName);
				OutCpp.Log(TEXT("\t\treturn Parms.ReturnValue;") LINE_TERMINATOR);
			}
			OutCpp.Log(TEXT("\t}") LINE_TERMINATOR);
		}
	}

	WriteMacro(OutGeneratedHeaderText, CallbackWrappersMacroName, RPCWrappers);
}


/**
 * Determines if the property has alternate export text associated with it and if so replaces the text in PropertyText with the
 * alternate version. (for example, structs or properties that specify a native type using export-text).  Should be called immediately
 * after ExportCppDeclaration()
 *
 * @param	Prop			the property that is being exported
 * @param	PropertyText	the string containing the text exported from ExportCppDeclaration
 */
void FNativeClassHeaderGenerator::ApplyAlternatePropertyExportText(UProperty* Prop, FUHTStringBuilder& PropertyText, EExportingState ExportingState)
{
	UArrayProperty* ArrayProperty = Cast<UArrayProperty>(Prop);
	UProperty* InnerProperty = ArrayProperty ? ArrayProperty->Inner : nullptr;
	if (InnerProperty && (
			(InnerProperty->IsA<UByteProperty>() && ((UByteProperty*)InnerProperty)->Enum && FClass::IsDynamic(((UByteProperty*)InnerProperty)->Enum)) ||
			(InnerProperty->IsA<UEnumProperty>()                                          && FClass::IsDynamic(((UEnumProperty*)InnerProperty)->Enum))
		)
	)
	{
		const FString Original = InnerProperty->GetCPPType();
		const FString RawByte = InnerProperty->GetCPPType(nullptr, EPropertyExportCPPFlags::CPPF_BlueprintCppBackend);
		if (Original != RawByte)
		{
			PropertyText.ReplaceInline(*Original, *RawByte, ESearchCase::CaseSensitive);
		}
		return;
	}

	if (ExportingState == EExportingState::TypeEraseDelegates)
	{
		UDelegateProperty* DelegateProperty = Cast<UDelegateProperty>(Prop);
		UMulticastDelegateProperty* MulticastDelegateProperty = Cast<UMulticastDelegateProperty>(Prop);
		if (DelegateProperty || MulticastDelegateProperty)
		{
			FString Original = Prop->GetCPPType();
			FString PlaceholderOfSameSizeAndAlignemnt;
			if (DelegateProperty)
			{
				PlaceholderOfSameSizeAndAlignemnt = TEXT("FScriptDelegate");
			}
			else
			{
				PlaceholderOfSameSizeAndAlignemnt = TEXT("FMulticastScriptDelegate");
			}
			PropertyText.ReplaceInline(*Original, *PlaceholderOfSameSizeAndAlignemnt, ESearchCase::CaseSensitive);
		}
	}
}

void GetSourceFilesInDependencyOrderRecursive(TArray<FUnrealSourceFile*>& OutTest, const UPackage* Package, FUnrealSourceFile* SourceFile, TSet<const FUnrealSourceFile*>& VisitedSet, bool bCheckDependenciesOnly, const TSet<FUnrealSourceFile*>& Ignore)
{
	// Check if the Class has already been exported, after we've checked for circular header dependencies.
	if (OutTest.Contains(SourceFile) || Ignore.Contains(SourceFile))
	{
		return;
	}

	// Check for circular dependencies.
	if (VisitedSet.Contains(SourceFile))
	{
		UE_LOG(LogCompile, Error, TEXT("Circular dependency detected for filename %s!"), *SourceFile->GetFilename());
		return;
	}

	// Check for circular header dependencies between export classes.
	bCheckDependenciesOnly = bCheckDependenciesOnly || SourceFile->GetPackage() != Package;

	VisitedSet.Add(SourceFile);
	for (FHeaderProvider& Include : SourceFile->GetIncludes())
	{
		if (FUnrealSourceFile* IncludeFile = Include.Resolve())
		{
			GetSourceFilesInDependencyOrderRecursive(OutTest, Package, IncludeFile, VisitedSet, bCheckDependenciesOnly, Ignore);
		}
	}
	VisitedSet.Remove(SourceFile);

	if (!bCheckDependenciesOnly)
	{
		OutTest.Add(SourceFile);
	}
}

TArray<FUnrealSourceFile*> GetSourceFilesInDependencyOrder(const UPackage* Package, const TArray<FUnrealSourceFile*>& SourceFiles, const TSet<FUnrealSourceFile*>& Ignore)
{
	TArray<FUnrealSourceFile*> Result;
	TSet<const FUnrealSourceFile*>	VisitedSet;
	for (FUnrealSourceFile* SourceFile : SourceFiles)
	{
		if (SourceFile->GetPackage() == Package)
		{
			GetSourceFilesInDependencyOrderRecursive(Result, Package, SourceFile, VisitedSet, false, Ignore);
		}
	}

	return Result;
}

// Constructor.
FNativeClassHeaderGenerator::FNativeClassHeaderGenerator(
	const UPackage* InPackage,
	const TArray<FUnrealSourceFile*>& SourceFiles,
	FClasses& AllClasses,
	bool InAllowSaveExportedHeaders,
	FUHTMakefile& InUHTMakefile
)
	: API                        (FPackageName::GetShortName(InPackage).ToUpper())
	, Package                    (InPackage)
	, bAllowSaveExportedHeaders  (InAllowSaveExportedHeaders)
	, bFailIfGeneratedCodeChanges(FParse::Param(FCommandLine::Get(), TEXT("FailIfGeneratedCodeChanges")))
	, UHTMakefile                (InUHTMakefile)
{
	const FString PackageName = FPackageName::GetShortName(Package);

	bool bWriteClassesH = false;
	const bool bPackageHasAnyExportClasses = AllClasses.GetClassesInPackage(Package).ContainsByPredicate([](FClass* Class)
	{
		return Class->HasAnyClassFlags(CLASS_Native) && !Class->HasAnyClassFlags(CLASS_NoExport | CLASS_Intrinsic);
	});
	if (bPackageHasAnyExportClasses)
	{
		for (FUnrealSourceFile* SourceFile : SourceFiles)
		{
			TArray<UClass*> DefinedClasses = SourceFile->GetDefinedClasses();
			for (UClass* Class : DefinedClasses)
			{
				if (!Class->HasAnyClassFlags(CLASS_Native))
				{
					Class->UnMark(EObjectMark(OBJECTMARK_TagImp | OBJECTMARK_TagExp));
				}
				else if (GTypeDefinitionInfoMap.Contains(Class) && !Class->HasAnyClassFlags(CLASS_NoExport))
				{
					bWriteClassesH = true;
					Class->UnMark(OBJECTMARK_TagImp);
					Class->Mark(OBJECTMARK_TagExp);
				}
			}
		}
	}

	// Export an include line for each header
	TArray<FUnrealSourceFile*> PublicHeaderGroupIncludes;
	FUHTStringBuilder GeneratedFunctionDeclarations;

	TArray<FUnrealSourceFile*> Exported;
	{
		// Get source files and ignore them next time round
		static TSet<FUnrealSourceFile*> ExportedSourceFiles;
		Exported = GetSourceFilesInDependencyOrder(Package, SourceFiles, ExportedSourceFiles);
		ExportedSourceFiles.Append(Exported);
	}

	/** Generated function implementations that belong in the cpp file, split into multiple files base on line count **/
	TArray<TUniqueObj<FUHTStringBuilderLineCounter>> GeneratedFunctionBodyTextSplit;
	auto GetGeneratedFunctionTextDevice = [&GeneratedFunctionBodyTextSplit]() -> FUHTStringBuilder&
	{
		static struct FMaxLinesPerCpp
		{
			int32 FirstValue;
			int32 OtherValue;
			FMaxLinesPerCpp()
			{
				FirstValue = 5000;
				check(GConfig);
				GConfig->GetInt(TEXT("UnrealHeaderTool"), TEXT("MaxLinesPerInitialCpp"), FirstValue, GEngineIni);

	#if ( PLATFORM_WINDOWS && defined(__clang__) )	// @todo clang: Clang r231657 often crashes with huge Engine.generated.cpp files, so we split using a smaller threshold
				OtherValue = 15000;
	#else
				// We do this only for non-clang builds for now
				OtherValue = 60000;
				GConfig->GetInt(TEXT("UnrealHeaderTool"), TEXT("MaxLinesPerCpp"), OtherValue, GEngineIni);
	#endif
			}
		} MaxLinesPerCpp;

		if (GeneratedFunctionBodyTextSplit.Num() == 0 || (GeneratedFunctionBodyTextSplit.Num() == 1 && GeneratedFunctionBodyTextSplit[0]->GetLineCount() > MaxLinesPerCpp.FirstValue) || (GeneratedFunctionBodyTextSplit.Last()->GetLineCount() > MaxLinesPerCpp.OtherValue))
		{
			GeneratedFunctionBodyTextSplit.Emplace();
		}

		return GeneratedFunctionBodyTextSplit.Last().Get();
	};

	for (FUnrealSourceFile* SourceFile : Exported)
	{
		FUHTStringBuilder GeneratedHeaderText;

		NameLookupCPP.SetCurrentSourceFile(SourceFile);
		UHTMakefile.AddToHeaderOrder(SourceFile);

		TArray<UEnum*>             Enums;
		TArray<UScriptStruct*>     Structs;
		TArray<UDelegateFunction*> DelegateFunctions;
		SourceFile->GetScope()->SplitTypesIntoArrays(Enums, Structs, DelegateFunctions);

		// Reverse the containers as they come out in the reverse order of declaration
		Algo::Reverse(Enums);
		Algo::Reverse(Structs);
		Algo::Reverse(DelegateFunctions);

		GeneratedHeaderText.Logf(
			TEXT("#ifdef %s")																	LINE_TERMINATOR
			TEXT("#error \"%s.generated.h already included, missing '#pragma once' in %s.h\"")	LINE_TERMINATOR
			TEXT("#endif")																		LINE_TERMINATOR
			TEXT("#define %s")																	LINE_TERMINATOR
			LINE_TERMINATOR,
			*SourceFile->GetFileDefineName(), *SourceFile->GetStrippedFilename(), *SourceFile->GetStrippedFilename(), *SourceFile->GetFileDefineName());

		ExportAutoIncludes(GeneratedHeaderText, *SourceFile);

		// export delegate definitions
		for (UDelegateFunction* Func : DelegateFunctions)
		{
			ExportDelegateDeclaration(GetGeneratedFunctionTextDevice(), GeneratedFunctionDeclarations, *SourceFile, Func);
		}

		// Export enums declared in non-UClass headers.
		for (UEnum* Enum : Enums)
		{
			// Is this ever not the case?
			if (Enum->GetOuter()->IsA(UPackage::StaticClass()))
			{
				ExportGeneratedEnumInitCode(GetGeneratedFunctionTextDevice(), GeneratedFunctionDeclarations, *SourceFile, Enum);
			}
		}

		// export boilerplate macros for structs
		// reverse the order.
		for (UScriptStruct* Struct : Structs)
		{
			ExportGeneratedStructBodyMacros(GeneratedHeaderText, GetGeneratedFunctionTextDevice(), GeneratedFunctionDeclarations, *SourceFile, Struct);
		}

		// export delegate wrapper function implementations
		for (UDelegateFunction* Func : DelegateFunctions)
		{
			ExportDelegateDefinition(GeneratedHeaderText, *SourceFile, Func);
		}

		TArray<UClass*> DefinedClasses = SourceFile->GetDefinedClasses();
		for (UClass* Class : DefinedClasses)
		{
			if (!(Class->ClassFlags & CLASS_Intrinsic))
			{
				ExportClassFromSourceFileInner(GeneratedHeaderText, GetGeneratedFunctionTextDevice(), GeneratedFunctionDeclarations, (FClass*)Class, *SourceFile);
			}
		}

		GeneratedHeaderText.Log(TEXT("#undef CURRENT_FILE_ID\r\n"));
		GeneratedHeaderText.Logf(TEXT("#define CURRENT_FILE_ID %s\r\n\r\n\r\n"), *SourceFile->GetFileId());

		for (UEnum* Enum : Enums)
		{
			ExportEnum(GeneratedHeaderText, Enum);
		}

		const FString PkgName = FPackageName::GetShortName(Package);

		FString PkgDir;
		FString GeneratedIncludeDirectory;
		if (!FindPackageLocation(*PkgName, PkgDir, GeneratedIncludeDirectory))
		{
			UE_LOG(LogCompile, Error, TEXT("Failed to find path for package %s"), *PkgName);
		}

		const FString ClassHeaderPath = GeneratedIncludeDirectory / FPaths::GetBaseFilename(SourceFile->GetFilename()) + TEXT(".generated.h");

		bool bHasChanged = WriteHeader(*ClassHeaderPath, GeneratedHeaderText, ForwardDeclarations);

		SourceFile->SetGeneratedFilename(ClassHeaderPath);
		SourceFile->SetHasChanged(bHasChanged);

		ForwardDeclarations.Reset();

		if (GPublicSourceFileSet.Contains(SourceFile))
		{
			PublicHeaderGroupIncludes.AddUnique(SourceFile);
		}
	}

	if (bWriteClassesH)
	{
		// Write the classes and enums header prefixes.

		FString PkgDir;
		FString GeneratedIncludeDirectory;
		if (!FindPackageLocation(*PackageName, PkgDir, GeneratedIncludeDirectory))
		{
			UE_LOG(LogCompile, Error, TEXT("Failed to find path for package %s"), *PackageName);
		}

		FUHTStringBuilder ClassesHText;
		ClassesHText.Log(HeaderCopyright);
		ClassesHText.Log(TEXT("#pragma once\r\n"));
		ClassesHText.Log(TEXT("\r\n"));
		ClassesHText.Log(TEXT("\r\n"));

		// Fill with the rest source files from this package.
		for (FUnrealSourceFile* SourceFile : GPublicSourceFileSet)
		{
			if (SourceFile->GetPackage() == InPackage)
			{
				PublicHeaderGroupIncludes.AddUnique(SourceFile);
			}
		}

		for (FUnrealSourceFile* SourceFile : PublicHeaderGroupIncludes)
		{
			ClassesHText.Logf(TEXT("#include \"%s\"") LINE_TERMINATOR, *GetBuildPath(*SourceFile));
		}

		ClassesHText.Log(LINE_TERMINATOR);

		// Save the classes header if it has changed.
		FString ClassesHeaderPath = GeneratedIncludeDirectory / (PackageName + TEXT("Classes.h"));
		SaveHeaderIfChanged(*ClassesHeaderPath, *ClassesHText);
	}

	// now export the names for the functions in this package
	// notice we always export this file (as opposed to only exporting if we have any marked names)
	// because there would be no way to know when the file was created otherwise
	// Export .generated.cpp
	UE_LOG(LogCompile, Log, TEXT("Autogenerating boilerplate cpp: %s.generated.cpp"), *PackageName);

	if (GeneratedFunctionDeclarations.Len() || UniqueCrossModuleReferences.Num() > 0)
	{
		uint32 CombinedCRC = 0;
		for (TUniqueObj<FUHTStringBuilderLineCounter>& Split : GeneratedFunctionBodyTextSplit)
		{
			uint32 SplitCRC = GenerateTextCRC(**Split);
			if (CombinedCRC == 0)
			{
				// Don't combine in the first case because it keeps GUID backwards compatibility
				CombinedCRC = SplitCRC;
			}
			else
			{
				CombinedCRC = HashCombine(SplitCRC, CombinedCRC);
			}
		}

		ExportGeneratedPackageInitCode(GetGeneratedFunctionTextDevice(), GeneratedFunctionDeclarations, Package, CombinedCRC);
	}

	// Write out large include-everything header
	FUHTStringBuilder Includes;
	for (const FUnrealSourceFile* SourceFile : Exported)
	{
		FString NewFileName = SourceFile->GetFilename();
		ConvertToBuildIncludePath(Package, NewFileName);

		FString IncludeStr = FString::Printf(
			TEXT("#ifndef %s")			LINE_TERMINATOR
			TEXT("\t#include \"%s\"")	LINE_TERMINATOR
			TEXT("#endif")				LINE_TERMINATOR,
			*SourceFile->GetFileDefineName(), *NewFileName);

		Includes.Log(*IncludeStr);
	}

	const FManifestModule* ModuleInfo = GPackageToManifestModuleMap.FindChecked(Package);

	// Write out the ordered class dependencies into a single header that we can easily include
	FString DepHeaderPathname = ModuleInfo->GeneratedCPPFilenameBase + TEXT(".dep.h");
	SaveHeaderIfChanged(*DepHeaderPathname, *FString::Printf(TEXT("%s%s%s"), HeaderCopyright, RequiredCPPIncludes, *Includes));

	// Find other includes to put at the top of the .cpp
	FUHTStringBuilder OtherIncludes;
	if (ModuleInfo->PCH.Len())
	{
		FString PCH = ModuleInfo->PCH;
		ConvertToBuildIncludePath(Package, PCH);
		OtherIncludes.Logf(TEXT("#include \"%s\"") LINE_TERMINATOR, *PCH);
	}
	OtherIncludes.Logf(TEXT("#include \"%s\"") LINE_TERMINATOR, *FPaths::GetCleanFilename(DepHeaderPathname));

	{
		// Generate CPP files
		TArray<FString> NumberedHeaderNames;
		for (int32 FileIdx = 0; FileIdx < GeneratedFunctionBodyTextSplit.Num(); ++FileIdx)
		{
			FUHTStringBuilder FileText;
			ExportGeneratedCPP(
				FileText,
				*FString::Printf(TEXT("%d%s"), FileIdx + 1, *ModuleInfo->Name),
				*GeneratedFunctionDeclarations,
				*GeneratedFunctionBodyTextSplit[FileIdx].Get(),
				*OtherIncludes
			);

			FString CppPath = ModuleInfo->GeneratedCPPFilenameBase + (GeneratedFunctionBodyTextSplit.Num() > 1 ? *FString::Printf(TEXT(".%d.cpp"), FileIdx + 1) : TEXT(".cpp"));
			SaveHeaderIfChanged(*CppPath, *FileText);

			if (GeneratedFunctionBodyTextSplit.Num() > 1)
			{
				NumberedHeaderNames.Add(FPaths::GetCleanFilename(CppPath));
			}
		}

		if (bAllowSaveExportedHeaders)
		{
			// Delete old generated .cpp files which we don't need because we generated less code than last time.
			TArray<FString> FoundFiles;
			IFileManager::Get().FindFiles(FoundFiles, *(ModuleInfo->GeneratedCPPFilenameBase + TEXT(".*.cpp")), true, false);
			FString BaseDir = FPaths::GetPath(ModuleInfo->GeneratedCPPFilenameBase);
			for (FString& File : FoundFiles)
			{
				if (!NumberedHeaderNames.Contains(File))
				{
					IFileManager::Get().Delete(*FPaths::Combine(*BaseDir, *File));
				}
			}

			// delete the old .cpp file that will cause link errors if it's left around (Engine.generated.cpp and Engine.generated.1.cpp will 
			// conflict now that we no longer use Engine.generated.cpp to #include Engine.generated.1.cpp, and UBT would compile all 3)
			// @todo: This is a temp measure so we don't force everyone to require a Clean
			if (GeneratedFunctionBodyTextSplit.Num() > 1)
			{
				FString CppPath = ModuleInfo->GeneratedCPPFilenameBase + TEXT(".cpp");
				IFileManager::Get().Delete(*CppPath);
			}
		}
	}

	// Export all changed headers from their temp files to the .h files
	ExportUpdatedHeaders(PackageName);

	// Delete stale *.generated.h files
	DeleteUnusedGeneratedHeaders();
}

void FNativeClassHeaderGenerator::DeleteUnusedGeneratedHeaders()
{
	TSet<FString> AllIntermediateFolders;
	TSet<FString> PackageHeaderPathSet(PackageHeaderPaths);

	for (const FString& PackageHeader : PackageHeaderPaths)
	{
		const FString IntermediatePath = FPaths::GetPath(PackageHeader);

		if (AllIntermediateFolders.Contains(IntermediatePath))
		{
			continue;
		}

		AllIntermediateFolders.Add( IntermediatePath );

		TArray<FString> AllHeaders;
		IFileManager::Get().FindFiles( AllHeaders, *(IntermediatePath / TEXT("*.generated.h")), true, false );

		for (const FString& Header : AllHeaders)
		{
			const FString HeaderPath = IntermediatePath / Header;

			if (PackageHeaderPathSet.Contains(HeaderPath))
			{
				continue;
			}

			// Check intrinsic classes. Get the class name from file name by removing .generated.h.
			const FString HeaderFilename = FPaths::GetBaseFilename(HeaderPath);
			const int32   GeneratedIndex = HeaderFilename.Find(TEXT(".generated"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
			const FString ClassName      = HeaderFilename.Mid(0, GeneratedIndex);
			UClass* IntrinsicClass       = FindObject<UClass>(ANY_PACKAGE, *ClassName);
			if (!IntrinsicClass || !IntrinsicClass->HasAnyClassFlags(CLASS_Intrinsic))
			{
				IFileManager::Get().Delete(*HeaderPath);
			}
		}
	}
}

/**
 * Dirty hack global variable to allow different result codes passed through
 * exceptions. Needs to be fixed in future versions of UHT.
 */
ECompilationResult::Type GCompilationResult = ECompilationResult::OtherCompilationError;

bool FNativeClassHeaderGenerator::SaveHeaderIfChanged(const TCHAR* HeaderPath, const TCHAR* InNewHeaderContents)
{
	if ( !bAllowSaveExportedHeaders )
	{
		// Return false indicating that the header did not need updating
		return false;
	}

	const TCHAR* NewHeaderContents = InNewHeaderContents;
	static bool bTestedCmdLine = false;
	if (!bTestedCmdLine)
	{
		bTestedCmdLine = true;

		const FString ReferenceGeneratedCodePath = FPaths::GameSavedDir() / TEXT("ReferenceGeneratedCode/");
		const FString VerifyGeneratedCodePath = FPaths::GameSavedDir() / TEXT("VerifyGeneratedCode/");

		if (FParse::Param(FCommandLine::Get(), TEXT("WRITEREF")))
		{
			bWriteContents = true;
			UE_LOG(LogCompile, Log, TEXT("********************************* Writing reference generated code to %s."), *ReferenceGeneratedCodePath);
			UE_LOG(LogCompile, Log, TEXT("********************************* Deleting all files in ReferenceGeneratedCode."));
			IFileManager::Get().DeleteDirectory(*ReferenceGeneratedCodePath, false, true);
			IFileManager::Get().MakeDirectory(*ReferenceGeneratedCodePath);
		}
		else if (FParse::Param( FCommandLine::Get(), TEXT("VERIFYREF")))
		{
			bVerifyContents = true;
			UE_LOG(LogCompile, Log, TEXT("********************************* Writing generated code to %s and comparing to %s"), *VerifyGeneratedCodePath, *ReferenceGeneratedCodePath);
			UE_LOG(LogCompile, Log, TEXT("********************************* Deleting all files in VerifyGeneratedCode."));
			IFileManager::Get().DeleteDirectory(*VerifyGeneratedCodePath, false, true);
			IFileManager::Get().MakeDirectory(*VerifyGeneratedCodePath);
		}
	}

	if (bWriteContents || bVerifyContents)
	{
		FString Ref    = FPaths::GameSavedDir() / TEXT("ReferenceGeneratedCode") / FPaths::GetCleanFilename(HeaderPath);
		FString Verify = FPaths::GameSavedDir() / TEXT("VerifyGeneratedCode") / FPaths::GetCleanFilename(HeaderPath);

		if (bWriteContents)
		{
			int32 i;
			for (i = 0 ;i < 10; i++)
			{
				if (FFileHelper::SaveStringToFile(NewHeaderContents, *Ref))
				{
					break;
				}
				FPlatformProcess::Sleep(1.0f); // I don't know why this fails after we delete the directory
			}
			check(i<10);
		}
		else
		{
			int32 i;
			for (i = 0 ;i < 10; i++)
			{
				if (FFileHelper::SaveStringToFile(NewHeaderContents, *Verify))
				{
					break;
				}
				FPlatformProcess::Sleep(1.0f); // I don't know why this fails after we delete the directory
			}
			check(i<10);
			FString RefHeader;
			FString Message;
			if (!FFileHelper::LoadFileToString(RefHeader, *Ref))
			{
				Message = FString::Printf(TEXT("********************************* %s appears to be a new generated file."), *FPaths::GetCleanFilename(HeaderPath));
			}
			else
			{
				if (FCString::Strcmp(NewHeaderContents, *RefHeader) != 0)
				{
					Message = FString::Printf(TEXT("********************************* %s has changed."), *FPaths::GetCleanFilename(HeaderPath));
				}
			}
			if (Message.Len())
			{
				UE_LOG(LogCompile, Log, TEXT("%s"), *Message);
				ChangeMessages.AddUnique(Message);
			}
		}
	}


	FString OriginalHeaderLocal;
	FFileHelper::LoadFileToString(OriginalHeaderLocal, HeaderPath);

	const bool bHasChanged = OriginalHeaderLocal.Len() == 0 || FCString::Strcmp(*OriginalHeaderLocal, NewHeaderContents);
	if (bHasChanged)
	{
		if (bFailIfGeneratedCodeChanges)
		{
			FString ConflictPath = FString(HeaderPath) + TEXT(".conflict");
			FFileHelper::SaveStringToFile(NewHeaderContents, *ConflictPath);

			GCompilationResult = ECompilationResult::FailedDueToHeaderChange;
			FError::Throwf(TEXT("ERROR: '%s': Changes to generated code are not allowed - conflicts written to '%s'"), HeaderPath, *ConflictPath);
		}

		// save the updated version to a tmp file so that the user can see what will be changing
		const FString TmpHeaderFilename = GenerateTempHeaderName( HeaderPath, false );

		// delete any existing temp file
		IFileManager::Get().Delete( *TmpHeaderFilename, false, true );
		if ( !FFileHelper::SaveStringToFile(NewHeaderContents, *TmpHeaderFilename) )
		{
			UE_LOG_WARNING_UHT(TEXT("Failed to save header export preview: '%s'"), *TmpHeaderFilename);
		}

		TempHeaderPaths.Add(TmpHeaderFilename);
	}

	// Remember this header filename to be able to check for any old (unused) headers later.
	PackageHeaderPaths.Add(FString(HeaderPath).Replace(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive));

	return bHasChanged;
}

/**
* Create a temp header file name from the header name
*
* @param	CurrentFilename		The filename off of which the current filename will be generated
* @param	bReverseOperation	Get the header from the temp file name instead
*
* @return	The generated string
*/
FString FNativeClassHeaderGenerator::GenerateTempHeaderName( FString CurrentFilename, bool bReverseOperation )
{
	return bReverseOperation
		? CurrentFilename.Replace(TEXT(".tmp"), TEXT(""))
		: CurrentFilename + TEXT(".tmp");
}

/** 
* Exports the temp header files into the .h files, then deletes the temp files.
* 
* @param	PackageName	Name of the package being saved
*/
void FNativeClassHeaderGenerator::ExportUpdatedHeaders(FString PackageName)
{
	for (const FString& TmpFilename : TempHeaderPaths)
	{
		FString Filename = GenerateTempHeaderName( TmpFilename, true );
		if (!IFileManager::Get().Move(*Filename, *TmpFilename, true, true))
		{
			UE_LOG(LogCompile, Error, TEXT("Error exporting %s: couldn't write file '%s'"), *PackageName, *Filename);
		}
		else
		{
			UE_LOG(LogCompile, Log, TEXT("Exported updated C++ header: %s"), *Filename);
		}
	}
}

/**
 * Exports C++ definitions for boilerplate that was generated for a package.
 * They are exported to a file using the name <PackageName>.generated.cpp
 */
void FNativeClassHeaderGenerator::ExportGeneratedCPP(FOutputDevice& Out, const TCHAR* EmptyLinkFunctionPostfix, const TCHAR* Declarations, const TCHAR* Body, const TCHAR* OtherIncludes)
{
	static const TCHAR EnableOptimization        [] = TEXT("PRAGMA_ENABLE_OPTIMIZATION") LINE_TERMINATOR;
	static const TCHAR DisableOptimization       [] = TEXT("PRAGMA_DISABLE_OPTIMIZATION") LINE_TERMINATOR;
	static const TCHAR EnableDeprecationWarnings [] = TEXT("PRAGMA_ENABLE_DEPRECATION_WARNINGS") LINE_TERMINATOR;
	static const TCHAR DisableDeprecationWarnings[] = TEXT("PRAGMA_DISABLE_DEPRECATION_WARNINGS") LINE_TERMINATOR;
	static const TCHAR DisableWarning4883        [] = TEXT("#ifdef _MSC_VER") LINE_TERMINATOR TEXT("#pragma warning (push)") LINE_TERMINATOR TEXT("#pragma warning (disable : 4883)") LINE_TERMINATOR TEXT("#endif") LINE_TERMINATOR;
	static const TCHAR EnableWarning4883         [] = TEXT("#ifdef _MSC_VER") LINE_TERMINATOR TEXT("#pragma warning (pop)") LINE_TERMINATOR TEXT("#endif") LINE_TERMINATOR;

	Out.Log(HeaderCopyright);
	Out.Log(RequiredCPPIncludes);
	Out.Log(OtherIncludes);
	Out.Log(DisableOptimization);
	Out.Log(DisableWarning4883);
	Out.Log(DisableDeprecationWarnings);

	Out.Logf(TEXT("void EmptyLinkFunctionForGeneratedCode%s() {}") LINE_TERMINATOR, EmptyLinkFunctionPostfix);

	if (*Declarations || UniqueCrossModuleReferences.Num() > 0)
	{
		Out.Logf(TEXT("#if USE_COMPILED_IN_NATIVES\r\n"));
		if (UniqueCrossModuleReferences.Num() > 0)
		{
			Out.Logf(TEXT("// Cross Module References\r\n"));
			for (const FString& Ref : UniqueCrossModuleReferences)
			{
				Out.Log(*Ref);
			}
			Out.Logf(TEXT("\r\n"));
		}
		Out.Log(Declarations);
		Out.Log(Body);
		Out.Logf(TEXT("#endif\r\n"));
	}

	Out.Log(EnableDeprecationWarnings);
	Out.Log(EnableWarning4883);
	Out.Log(EnableOptimization);
}

/** Get all script plugins based on ini setting */
void GetScriptPlugins(TArray<IScriptGeneratorPluginInterface*>& ScriptPlugins)
{
	FScopedDurationTimer PluginTimeTracker(GPluginOverheadTime);

	ScriptPlugins = IModularFeatures::Get().GetModularFeatureImplementations<IScriptGeneratorPluginInterface>(TEXT("ScriptGenerator"));
	UE_LOG(LogCompile, Log, TEXT("Found %d script generator plugins."), ScriptPlugins.Num());

	// Check if we can use these plugins and initialize them
	for (int32 PluginIndex = ScriptPlugins.Num() - 1; PluginIndex >= 0; --PluginIndex)
	{
		auto ScriptGenerator = ScriptPlugins[PluginIndex];
		bool bSupportedPlugin = ScriptGenerator->SupportsTarget(GManifest.TargetName);
		if (bSupportedPlugin)
		{
			// Find the right output directory for this plugin base on its target (Engine-side) plugin name.
			FString GeneratedCodeModuleName = ScriptGenerator->GetGeneratedCodeModuleName();
			const FManifestModule* GeneratedCodeModule = NULL;
			FString OutputDirectory;
			FString IncludeBase;
			for (const FManifestModule& Module : GManifest.Modules)
			{
				if (Module.Name == GeneratedCodeModuleName)
				{
					GeneratedCodeModule = &Module;
				}
			}
			if (GeneratedCodeModule)
			{
				UE_LOG(LogCompile, Log, TEXT("Initializing script generator \'%s\'"), *ScriptGenerator->GetGeneratorName());
				ScriptGenerator->Initialize(GManifest.RootLocalPath, GManifest.RootBuildPath, GeneratedCodeModule->GeneratedIncludeDirectory, GeneratedCodeModule->IncludeBase);
			}
			else
			{
				// Can't use this plugin
				UE_LOG(LogCompile, Log, TEXT("Unable to determine output directory for %s. Cannot export script glue with \'%s\'"), *GeneratedCodeModuleName, *ScriptGenerator->GetGeneratorName());
				bSupportedPlugin = false;				
			}
		}
		if (!bSupportedPlugin)
		{
			UE_LOG(LogCompile, Log, TEXT("Script generator \'%s\' not supported for target: %s"), *ScriptGenerator->GetGeneratorName(), *GManifest.TargetName);
			ScriptPlugins.RemoveAt(PluginIndex);
		}
	}
}

/**
 * Tries to resolve super classes for classes defined in the given
 * module.
 *
 * @param Package Modules package.
 */
void ResolveSuperClasses(UPackage* Package)
{
	TArray<UObject*> Objects;
	GetObjectsWithOuter(Package, Objects);

	for (auto* Object : Objects)
	{
		if (!Object->IsA<UClass>())
		{
			continue;
		}

		UClass* DefinedClass = Cast<UClass>(Object);

		if (DefinedClass->HasAnyClassFlags(CLASS_Intrinsic | CLASS_NoExport))
		{
			continue;
		}

		const FSimplifiedParsingClassInfo& ParsingInfo = GTypeDefinitionInfoMap[DefinedClass]->GetUnrealSourceFile()
			.GetDefinedClassParsingInfo(DefinedClass);

		const FString& BaseClassNameStripped = GetClassNameWithPrefixRemoved(ParsingInfo.GetBaseClassName());

		if (!BaseClassNameStripped.IsEmpty() && !DefinedClass->GetSuperClass())
		{
			UClass* FoundBaseClass = FindObject<UClass>(Package, *BaseClassNameStripped);

			if (FoundBaseClass == nullptr)
			{
				FoundBaseClass = FindObject<UClass>(ANY_PACKAGE, *BaseClassNameStripped);
			}

			if (FoundBaseClass == nullptr)
			{
				// Don't know its parent class. Raise error.
				FError::Throwf(TEXT("Couldn't find parent type for '%s' named '%s' in current module or any other module parsed so far."),
					*DefinedClass->GetName(), *ParsingInfo.GetBaseClassName());
			}

			DefinedClass->SetSuperStruct(FoundBaseClass);
			DefinedClass->ClassCastFlags |= FoundBaseClass->ClassCastFlags;
		}
	}
}

ECompilationResult::Type PreparseModules(FUHTMakefile& UHTMakefile, const FString& ModuleInfoPath, int32& NumFailures)
{
	// Three passes.  1) Public 'Classes' headers (legacy)  2) Public headers   3) Private headers
	enum EHeaderFolderTypes
	{
		PublicClassesHeaders = 0,
		PublicHeaders = 1,
		PrivateHeaders,

		FolderType_Count
	};

	ECompilationResult::Type Result = ECompilationResult::Succeeded;
	for (FManifestModule& Module : GManifest.Modules)
	{
		if (Result != ECompilationResult::Succeeded)
		{
			break;
		}

		FName ModuleName = FName(*Module.Name);
		UHTMakefile.SetCurrentModuleName(ModuleName);
		bool bLoadFromMakefile = UHTMakefile.CanLoadModule(Module);
		if (bLoadFromMakefile)
		{
			// Load module data from makefile.
			UHTMakefile.LoadModuleData(ModuleName, Module);
			continue;
		}
		UHTMakefile.AddModule(ModuleName);

		// Mark that we'll need to append newly constructed objects to ones loaded from makefile.
		UHTMakefile.SetShouldMoveNewObjects();

		// Force regeneration of all subsequent modules, otherwise data will get corrupted.
		Module.ForceRegeneration();

		UPackage* Package = Cast<UPackage>(StaticFindObjectFast(UPackage::StaticClass(), NULL, FName(*Module.LongPackageName), false, false));
		if (Package == NULL)
		{
			Package = CreatePackage(NULL, *Module.LongPackageName);
		}
		// Set some package flags for indicating that this package contains script
		// NOTE: We do this even if we didn't have to create the package, because CoreUObject is compiled into UnrealHeaderTool and we still
		//       want to make sure our flags get set
		Package->SetPackageFlags(PKG_ContainsScript | PKG_Compiling);
		Package->ClearPackageFlags(PKG_ClientOptional | PKG_ServerSideOnly);
		if (Module.ModuleType == EBuildModuleType::GameEditor || Module.ModuleType == EBuildModuleType::EngineEditor)
		{
			Package->SetPackageFlags(PKG_EditorOnly);
		}

		if (Module.ModuleType == EBuildModuleType::GameDeveloper || Module.ModuleType == EBuildModuleType::EngineDeveloper)
		{
			Package->SetPackageFlags(Package->GetPackageFlags() | PKG_Developer);
		}

		// Add new module or overwrite whatever we had loaded, that data is obsolete.
		UHTMakefile.AddPackage(Package);
		GPackageToManifestModuleMap.Add(Package, &Module);

		double ThisModulePreparseTime = 0.0;
		int32 NumHeadersPreparsed = 0;
		FDurationTimer ThisModuleTimer(ThisModulePreparseTime);
		ThisModuleTimer.Start();

		// Pre-parse the headers
		for (int32 PassIndex = 0; PassIndex < FolderType_Count && Result == ECompilationResult::Succeeded; ++PassIndex)
		{
			EHeaderFolderTypes CurrentlyProcessing = (EHeaderFolderTypes)PassIndex;

			// We'll make an ordered list of all UObject headers we care about.
			// @todo uht: Ideally 'dependson' would not be allowed from public -> private, or NOT at all for new style headers
			const TArray<FString>& UObjectHeaders =
				(CurrentlyProcessing == PublicClassesHeaders) ? Module.PublicUObjectClassesHeaders :
				(CurrentlyProcessing == PublicHeaders       ) ? Module.PublicUObjectHeaders        :
				                                                Module.PrivateUObjectHeaders;
			if (!UObjectHeaders.Num())
			{
				continue;
			}

			NumHeadersPreparsed += UObjectHeaders.Num();

			for (const FString& RawFilename : UObjectHeaders)
			{
			#if !PLATFORM_EXCEPTIONS_DISABLED
				try
			#endif
				{
					// Import class.
					const FString FullFilename = FPaths::ConvertRelativePathToFull(ModuleInfoPath, RawFilename);

					FString HeaderFile;
					if (!FFileHelper::LoadFileToString(HeaderFile, *FullFilename))
					{
						FError::Throwf(TEXT("UnrealHeaderTool was unable to load source file '%s'"), *FullFilename);
					}

					TSharedRef<FUnrealSourceFile> UnrealSourceFile = PerformInitialParseOnHeader(Package, *RawFilename, RF_Public | RF_Standalone, *HeaderFile, UHTMakefile);
					FUnrealSourceFile* UnrealSourceFilePtr = &UnrealSourceFile.Get();
					TArray<UClass*> DefinedClasses = UnrealSourceFile->GetDefinedClasses();
					for (UClass* DefinedClass : DefinedClasses)
					{
						UHTMakefile.AddClass(UnrealSourceFilePtr, DefinedClass);
					}
					GUnrealSourceFilesMap.Add(RawFilename, UnrealSourceFile);
					UHTMakefile.AddUnrealSourceFilesMapEntry(UnrealSourceFilePtr, RawFilename);

					if (CurrentlyProcessing == PublicClassesHeaders)
					{
						for (auto* Class : DefinedClasses)
						{
							UHTMakefile.AddPublicClassSetEntry(UnrealSourceFilePtr, Class);
						}

						GPublicSourceFileSet.Add(UnrealSourceFilePtr);
					}

					// Save metadata for the class path, both for it's include path and relative to the module base directory
					if (FullFilename.StartsWith(Module.BaseDirectory))
					{
						// Get the path relative to the module directory
						const TCHAR* ModuleRelativePath = *FullFilename + Module.BaseDirectory.Len();

						UnrealSourceFile->SetModuleRelativePath(ModuleRelativePath);

						// Calculate the include path
						const TCHAR* IncludePath = ModuleRelativePath;

						// Walk over the first potential slash
						if (*IncludePath == TEXT('/'))
						{
							IncludePath++;
						}

						// Does this module path start with a known include path location? If so, we can cut that part out of the include path
						static const TCHAR PublicFolderName[]  = TEXT("Public/");
						static const TCHAR PrivateFolderName[] = TEXT("Private/");
						static const TCHAR ClassesFolderName[] = TEXT("Classes/");
						if (FCString::Strnicmp(IncludePath, PublicFolderName, ARRAY_COUNT(PublicFolderName) - 1) == 0)
						{
							IncludePath += (ARRAY_COUNT(PublicFolderName) - 1);
						}
						else if (FCString::Strnicmp(IncludePath, PrivateFolderName, ARRAY_COUNT(PrivateFolderName) - 1) == 0)
						{
							IncludePath += (ARRAY_COUNT(PrivateFolderName) - 1);
						}
						else if (FCString::Strnicmp(IncludePath, ClassesFolderName, ARRAY_COUNT(ClassesFolderName) - 1) == 0)
						{
							IncludePath += (ARRAY_COUNT(ClassesFolderName) - 1);
						}

						// Add the include path
						if (*IncludePath != 0)
						{
							UnrealSourceFile->SetIncludePath(MoveTemp(IncludePath));
						}
					}
				}
			#if !PLATFORM_EXCEPTIONS_DISABLED
				catch (const FFileLineException& Ex)
				{
					TGuardValue<ELogTimes::Type> DisableLogTimes(GPrintLogTimes, ELogTimes::None);

					FString AbsFilename           = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*Ex.Filename);
					FString Prefix                = FString::Printf(TEXT("%s(%d): "), *AbsFilename, Ex.Line);
					FString FormattedErrorMessage = FString::Printf(TEXT("%sError: %s\r\n"), *Prefix, *Ex.Message);
					Result = GCompilationResult;

					UE_LOG(LogCompile, Log, TEXT("%s"), *FormattedErrorMessage);
					GWarn->Log(ELogVerbosity::Error, FormattedErrorMessage);

					++NumFailures;
				}
				catch (TCHAR* ErrorMsg)
				{
					TGuardValue<ELogTimes::Type> DisableLogTimes(GPrintLogTimes, ELogTimes::None);

					FString AbsFilename           = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*RawFilename);
					FString Prefix                = FString::Printf(TEXT("%s(1): "), *AbsFilename);
					FString FormattedErrorMessage = FString::Printf(TEXT("%sError: %s\r\n"), *Prefix, ErrorMsg);
					Result = GCompilationResult;

					UE_LOG(LogCompile, Log, TEXT("%s"), *FormattedErrorMessage);
					GWarn->Log(ELogVerbosity::Error, FormattedErrorMessage);

					++NumFailures;
				}
			#endif
			}
			if (Result == ECompilationResult::Succeeded && NumFailures != 0)
			{
				Result = ECompilationResult::OtherCompilationError;
			}
		}

		// Don't resolve superclasses for module when loading from makefile.
		// Data is only partially loaded at this point.
		if (!bLoadFromMakefile)
		{
	#if !PLATFORM_EXCEPTIONS_DISABLED
		try
	#endif
		{
			ResolveSuperClasses(Package);
		}
	#if !PLATFORM_EXCEPTIONS_DISABLED
		catch (TCHAR* ErrorMsg)
		{
			TGuardValue<ELogTimes::Type> DisableLogTimes(GPrintLogTimes, ELogTimes::None);

			FString FormattedErrorMessage = FString::Printf(TEXT("Error: %s\r\n"), ErrorMsg);

			Result = GCompilationResult;

			UE_LOG(LogCompile, Log, TEXT("%s"), *FormattedErrorMessage);
			GWarn->Log(ELogVerbosity::Error, FormattedErrorMessage);

			++NumFailures;
		}
	#endif

		ThisModuleTimer.Stop();
		UE_LOG(LogCompile, Log, TEXT("Preparsed module %s containing %i files(s) in %.2f secs."), *Module.LongPackageName, NumHeadersPreparsed, ThisModulePreparseTime);
	}
	}

	return Result;
}

ECompilationResult::Type UnrealHeaderTool_Main(const FString& ModuleInfoFilename)
{
	check(GIsUCCMakeStandaloneHeaderGenerator);
	ECompilationResult::Type Result = ECompilationResult::Succeeded;

	FString ModuleInfoPath = FPaths::GetPath(ModuleInfoFilename);

	// Load the manifest file, giving a list of all modules to be processed, pre-sorted by dependency ordering
#if !PLATFORM_EXCEPTIONS_DISABLED
	try
#endif
	{
		GManifest = FManifest::LoadFromFile(ModuleInfoFilename);
	}
#if !PLATFORM_EXCEPTIONS_DISABLED
	catch (const TCHAR* Ex)
	{
		UE_LOG(LogCompile, Error, TEXT("Failed to load manifest file '%s': %s"), *ModuleInfoFilename, Ex);
		return GCompilationResult;
	}
#endif

	// Counters.
	int32 NumFailures = 0;
	double TotalModulePreparseTime = 0.0;
	double TotalParseAndCodegenTime = 0.0;

	// Check if makefiles should be used. If not, only makefile serialization is skipped.
	// as the rest of code doesn't impact performance and we don't want to add ifs around
	// every makefile related piece of code.
	bool bUseMakefile = FParse::Param(FCommandLine::Get(), TEXT("UseMakefiles"));

	FUHTMakefile UHTMakefile;
	UHTMakefile.SetNameLookupCPP(&NameLookupCPP);
	UHTMakefile.SetManifest(&GManifest);

	// Declaring outside of bUseMakefile scope as the same value is used when saving makefile.
	FString MakefilePath;
	if (bUseMakefile)
	{
		MakefilePath = FPaths::Combine(*ModuleInfoPath, TEXT("UHT.makefile"));

		// If makefile failed to load, clear it
		if (!UHTMakefile.LoadFromFile(*MakefilePath, &GManifest))
		{
			UHTMakefile = FUHTMakefile();
		}
	}
	UHTMakefile.StartPreloading();
	{
		FDurationTimer TotalModulePreparseTimer(TotalModulePreparseTime);
		TotalModulePreparseTimer.Start();
		Result = PreparseModules(UHTMakefile, ModuleInfoPath, NumFailures);
		TotalModulePreparseTimer.Stop();
	}
	UHTMakefile.StopPreloading();
	// Do the actual parse of the headers and generate for them
	if (Result == ECompilationResult::Succeeded)
	{
		FScopedDurationTimer ParseAndCodeGenTimer(TotalParseAndCodegenTime);

		// Verify that all script declared superclasses exist.
		for (const UClass* ScriptClass : TObjectRange<UClass>())
		{
			const UClass* ScriptSuperClass = ScriptClass->GetSuperClass();

			if (ScriptSuperClass && !ScriptSuperClass->HasAnyClassFlags(CLASS_Intrinsic) && GTypeDefinitionInfoMap.Contains(ScriptClass) && !GTypeDefinitionInfoMap.Contains(ScriptSuperClass))
			{
				class FSuperClassContextSupplier : public FContextSupplier
				{
				public:
					FSuperClassContextSupplier(const UClass* Class)
						: DefinitionInfo(GTypeDefinitionInfoMap[Class])
					{ }

					virtual FString GetContext() override
					{
						FString Filename = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*DefinitionInfo->GetUnrealSourceFile().GetFilename());
						int32 LineNumber = DefinitionInfo->GetLineNumber();
						return FString::Printf(TEXT("%s(%i)"), *Filename, LineNumber);
					}
				private:
					TSharedRef<FUnrealTypeDefinitionInfo> DefinitionInfo;
				} ContextSupplier(ScriptClass);

				auto OldContext = GWarn->GetContext();

				TGuardValue<ELogTimes::Type> DisableLogTimes(GPrintLogTimes, ELogTimes::None);

				GWarn->SetContext(&ContextSupplier);
				GWarn->Log(ELogVerbosity::Error, FString::Printf(TEXT("Error: Superclass %s of class %s not found"), *ScriptSuperClass->GetName(), *ScriptClass->GetName()));
				GWarn->SetContext(OldContext);

				Result = ECompilationResult::OtherCompilationError;
				++NumFailures;
			}
		}

		if (Result == ECompilationResult::Succeeded)
		{
			TArray<IScriptGeneratorPluginInterface*> ScriptPlugins;
			// Can only export scripts for game targets
			if (GManifest.IsGameTarget)
			{
				GetScriptPlugins(ScriptPlugins);
			}

			if (UHTMakefile.ShouldMoveNewObjects())
			{
				UHTMakefile.MoveNewObjects();
			}

			for (const FManifestModule& Module : GManifest.Modules)
			{
				if (UPackage* Package = Cast<UPackage>(StaticFindObjectFast(UPackage::StaticClass(), NULL, FName(*Module.LongPackageName), false, false)))
				{
					// Object which represents all parsed classes
					FClasses AllClasses(Package);
					AllClasses.Validate();

					Result = FHeaderParser::ParseAllHeadersInside(AllClasses, GWarn, Package, Module, ScriptPlugins, UHTMakefile);
					if (Result != ECompilationResult::Succeeded)
					{
						++NumFailures;
						break;
					}
				}
			}

			{
				FScopedDurationTimer PluginTimeTracker(GPluginOverheadTime);
				for (IScriptGeneratorPluginInterface* ScriptGenerator : ScriptPlugins)
				{
					ScriptGenerator->FinishExport();
				}
			}

			// Get a list of external dependencies from each enabled plugin
			FString ExternalDependencies;
			for (IScriptGeneratorPluginInterface* ScriptPlugin : ScriptPlugins)
			{
				TArray<FString> PluginExternalDependencies;
				ScriptPlugin->GetExternalDependencies(PluginExternalDependencies);

				for (const FString& PluginExternalDependency : PluginExternalDependencies)
				{
					ExternalDependencies += PluginExternalDependency + LINE_TERMINATOR;
				}
			}
			FFileHelper::SaveStringToFile(ExternalDependencies, *GManifest.ExternalDependenciesFile);
		}
	}

	// Avoid TArray slack for meta data.
	GScriptHelper.Shrink();

	UE_LOG(LogCompile, Log, TEXT("Preparsing %i modules took %.2f seconds"), GManifest.Modules.Num(), TotalModulePreparseTime);
	UE_LOG(LogCompile, Log, TEXT("Parsing took %.2f seconds"), TotalParseAndCodegenTime - GHeaderCodeGenTime);
	UE_LOG(LogCompile, Log, TEXT("Code generation took %.2f seconds"), GHeaderCodeGenTime);
	UE_LOG(LogCompile, Log, TEXT("ScriptPlugin overhead was %.2f seconds"), GPluginOverheadTime);
	UE_LOG(LogCompile, Log, TEXT("Macroize time was %.2f seconds"), GMacroizeTime);

	if (bWriteContents)
	{
		UE_LOG(LogCompile, Log, TEXT("********************************* Wrote reference generated code to ReferenceGeneratedCode."));
	}
	else if (bVerifyContents)
	{
		UE_LOG(LogCompile, Log, TEXT("********************************* Wrote generated code to VerifyGeneratedCode and compared to ReferenceGeneratedCode"));
		for (FString& Msg : ChangeMessages)
		{
			UE_LOG(LogCompile, Error, TEXT("%s"), *Msg);
		}
		TArray<FString> RefFileNames;
		IFileManager::Get().FindFiles( RefFileNames, *(FPaths::GameSavedDir() / TEXT("ReferenceGeneratedCode/*.*")), true, false );
		TArray<FString> VerFileNames;
		IFileManager::Get().FindFiles( VerFileNames, *(FPaths::GameSavedDir() / TEXT("VerifyGeneratedCode/*.*")), true, false );
		if (RefFileNames.Num() != VerFileNames.Num())
		{
			UE_LOG(LogCompile, Error, TEXT("Number of generated files mismatch ref=%d, ver=%d"), RefFileNames.Num(), VerFileNames.Num());
		}
	}

	TheFlagAudit.WriteResults();

	GIsRequestingExit = true;

	if ((Result != ECompilationResult::Succeeded) || (NumFailures > 0))
	{
		// Makefile might be corrupted, it's safer to delete it now.
		IFileManager::Get().Delete(*MakefilePath);
		return ECompilationResult::OtherCompilationError;
	}
	
	if (bUseMakefile)
	{
		UHTMakefile.SaveToFile(*MakefilePath);
	}

	return Result;
}

UClass* ProcessParsedClass(bool bClassIsAnInterface, TArray<FHeaderProvider> &DependentOn, const FString& ClassName, const FString& BaseClassName, UObject* InParent, EObjectFlags Flags)
{
	FString ClassNameStripped = GetClassNameWithPrefixRemoved(*ClassName);

	// All classes must start with a valid unreal prefix
	if (!FHeaderParser::ClassNameHasValidPrefix(ClassName, ClassNameStripped))
	{
		FError::Throwf(TEXT("Invalid class name '%s'. The class name must have an appropriate prefix added (A for Actors, U for other classes)."), *ClassName);
	}

	// Ensure the base class has any valid prefix and exists as a valid class. Checking for the 'correct' prefix will occur during compilation
	FString BaseClassNameStripped;
	if (!BaseClassName.IsEmpty())
	{
		BaseClassNameStripped = GetClassNameWithPrefixRemoved(BaseClassName);
		if (!FHeaderParser::ClassNameHasValidPrefix(BaseClassName, BaseClassNameStripped))
		{
			FError::Throwf(TEXT("No prefix or invalid identifier for base class %s.\nClass names must match Unreal prefix specifications (e.g., \"UObject\" or \"AActor\")"), *BaseClassName);
		}

		if (DependentOn.ContainsByPredicate([&](const FHeaderProvider& Dependency){ FString DependencyStr = Dependency.GetId(); return !DependencyStr.Contains(TEXT(".generated.h")) && FPaths::GetBaseFilename(DependencyStr) == ClassNameStripped; }))
		{
			FError::Throwf(TEXT("Class '%s' contains a dependency (#include or base class) to itself"), *ClassName);
		}
	}

	//UE_LOG(LogCompile, Log, TEXT("Class: %s extends %s"),*ClassName,*BaseClassName);
	// Handle failure and non-class headers.
	if (BaseClassName.IsEmpty() && (ClassName != TEXT("UObject")))
	{
		FError::Throwf(TEXT("Class '%s' must inherit UObject or a UObject-derived class"), *ClassName);
	}

	if (ClassName == BaseClassName)
	{
		FError::Throwf(TEXT("Class '%s' cannot inherit from itself"), *ClassName);
	}

	// In case the file system and the class disagree on the case of the
	// class name replace the fname with the one from the script class file
	// This is needed because not all source control systems respect the
	// original filename's case
	FName ClassNameReplace(*ClassName, FNAME_Replace_Not_Safe_For_Threading);

	// Use stripped class name for processing and replace as we did above
	FName ClassNameStrippedReplace(*ClassNameStripped, FNAME_Replace_Not_Safe_For_Threading);

	UClass* ResultClass = FindObject<UClass>(InParent, *ClassNameStripped);

	// if we aren't generating headers, then we shouldn't set misaligned object, since it won't get cleared

	const static bool bVerboseOutput = FParse::Param(FCommandLine::Get(), TEXT("VERBOSE"));

	if (ResultClass == nullptr || !ResultClass->IsNative())
	{
		// detect if the same class name is used in multiple packages
		if (ResultClass == nullptr)
		{
			UClass* ConflictingClass = FindObject<UClass>(ANY_PACKAGE, *ClassNameStripped, true);
			if (ConflictingClass != nullptr)
			{
				UE_LOG_WARNING_UHT(TEXT("Duplicate class name: %s also exists in file %s"), *ClassName, *ConflictingClass->GetOutermost()->GetName());
			}
		}

		// Create new class.
		ResultClass = new(EC_InternalUseOnlyConstructor, InParent, *ClassNameStripped, Flags) UClass(FObjectInitializer(), nullptr);
		GClassHeaderNameWithNoPathMap.Add(ResultClass, ClassNameStripped);

		// add CLASS_Interface flag if the class is an interface
		// NOTE: at this pre-parsing/importing stage, we cannot know if our super class is an interface or not,
		// we leave the validation to the main header parser
		if (bClassIsAnInterface)
		{
			ResultClass->ClassFlags |= CLASS_Interface;
		}

		if (bVerboseOutput)
		{
			UE_LOG(LogCompile, Log, TEXT("Imported: %s"), *ResultClass->GetFullName());
		}
	}

	if (bVerboseOutput)
	{
		for (const auto& Dependency : DependentOn)
		{
			UE_LOG(LogCompile, Log, TEXT("\tAdding %s as a dependency"), *Dependency.ToString());
		}
	}

	return ResultClass;
}


TSharedRef<FUnrealSourceFile> PerformInitialParseOnHeader(UPackage* InParent, const TCHAR* FileName, EObjectFlags Flags, const TCHAR* Buffer, FUHTMakefile& UHTMakefile)
{
	const TCHAR* InBuffer = Buffer;

	// is the parsed class name an interface?
	bool bClassIsAnInterface = false;

	TArray<FHeaderProvider> DependsOn;

	// Parse the header to extract the information needed
	FUHTStringBuilder ClassHeaderTextStrippedOfCppText;
	TArray<FSimplifiedParsingClassInfo> ParsedClassArray;
	FHeaderParser::SimplifiedClassParse(FileName, Buffer, /*out*/ ParsedClassArray, /*out*/ DependsOn, ClassHeaderTextStrippedOfCppText);

	FUnrealSourceFile* UnrealSourceFilePtr = new FUnrealSourceFile(InParent, FileName, MoveTemp(ClassHeaderTextStrippedOfCppText));
	TSharedRef<FUnrealSourceFile> UnrealSourceFile = MakeShareable(UnrealSourceFilePtr);
	UHTMakefile.AddUnrealSourceFile(UnrealSourceFilePtr);
	UHTMakefile.AddToHeaderOrder(UnrealSourceFilePtr);
	for (auto& ParsedClassInfo : ParsedClassArray)
	{
		UClass* ResultClass = ProcessParsedClass(ParsedClassInfo.IsInterface(), DependsOn, ParsedClassInfo.GetClassName(), ParsedClassInfo.GetBaseClassName(), InParent, Flags);

		FScope::AddTypeScope(ResultClass, &UnrealSourceFile->GetScope().Get(), UnrealSourceFilePtr, UHTMakefile);

		AddTypeDefinition(UHTMakefile, UnrealSourceFilePtr, ResultClass, ParsedClassInfo.GetClassDefLine());
		UnrealSourceFile->AddDefinedClass(ResultClass, MoveTemp(ParsedClassInfo));
	}

	for (auto& DependsOnElement : DependsOn)
	{
		UnrealSourceFile->GetIncludes().AddUnique(DependsOnElement);
	}

	return UnrealSourceFile;
}

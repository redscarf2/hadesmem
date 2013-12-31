// Copyright (C) 2010-2013 Joshua Boyce.
// See the file COPYING for copying permission.

#include "imports.hpp"

#include <iostream>
#include <iterator>

#include <hadesmem/pelib/bound_import_dir.hpp>
#include <hadesmem/pelib/bound_import_dir_list.hpp>
#include <hadesmem/pelib/import_dir.hpp>
#include <hadesmem/pelib/import_dir_list.hpp>
#include <hadesmem/pelib/import_thunk.hpp>
#include <hadesmem/pelib/import_thunk_list.hpp>
#include <hadesmem/pelib/pe_file.hpp>
#include <hadesmem/process.hpp>

#include "main.hpp"

// TODO: Detect imports which simply point back to exports from the same module
// (also detect if the exports are forwarded, and also detect infinite loops).
// Remember that all exports can have the same name, so we need to use the hint
// first, then only use the name if we fail to find a match using the hint. See
// "Import name table" and "Import name hint" in ReversingLabs "Undocumented
// PECOFF" whitepaper for more information.

// TODO: Detect and handle cases where an import descriptor has a virtual
// terminator. See imports_vterm.exe from Corkami or "Import directory layout"
// in ReversingLabs "Undocumented PECOFF" whitepaper for more information.

// TODO: Support old style bound imports and bound forwarded imports.

// TODO: Are any fixes needed to properly support in-memory images, rather than
// just on-disk files?

namespace
{

bool HasBoundImportDir(hadesmem::Process const& process,
                       hadesmem::PeFile const& pe_file)
{
  hadesmem::NtHeaders const nt_headers(process, pe_file);
  // Intentionally not checking whether the RVA is valid, because we will detect
  // an empty list in that case, at which point we want to warn because an
  // invalid RVA is suspicious (even though it won't stop the file from
  // loading).
  return (nt_headers.GetNumberOfRvaAndSizes() >
            static_cast<int>(hadesmem::PeDataDir::BoundImport) &&
          nt_headers.GetDataDirectoryVirtualAddress(
            hadesmem::PeDataDir::BoundImport));
}

bool HasValidNonEmptyBoundImportDescList(hadesmem::Process const& process,
                                         hadesmem::PeFile const& pe_file)
{
  hadesmem::BoundImportDirList bound_import_dirs(process, pe_file);

  return (std::begin(bound_import_dirs) != std::end(bound_import_dirs));
}

void DumpImportThunk(hadesmem::ImportThunk const& thunk, bool is_bound)
{
  std::wcout << "\n";

  bool const by_ordinal = thunk.ByOrdinal();

  // This check needs to be first, because it's possible to have invalid data in
  // the IAT (i.e. -1) which will cause ByOrdinal to be true!
  if (is_bound)
  {
    std::wcout << "\t\t\tFunction: " << std::hex << thunk.GetFunction()
               << std::dec << "\n";
  }
  else if (by_ordinal)
  {
    std::wcout << "\t\t\tOrdinalRaw: " << std::hex << thunk.GetOrdinalRaw()
               << std::dec << "\n";
    std::wcout << "\t\t\tOrdinal: " << thunk.GetOrdinal() << "\n";
  }
  else
  {
    try
    {
      std::wcout << "\t\t\tAddressOfData: " << std::hex
                 << thunk.GetAddressOfData() << std::dec << "\n";
      std::wcout << "\t\t\tHint: " << thunk.GetHint() << "\n";
      std::wcout << "\t\t\tName: " << thunk.GetName().c_str() << "\n";
    }
    catch (std::exception const& /*e*/)
    {
      std::wcout << "\t\t\tWARNING! Invalid import thunk name data.\n";
      WarnForCurrentFile(WarningType::kUnsupported);
    }
  }
}
}

void DumpImports(hadesmem::Process const& process,
                 hadesmem::PeFile const& pe_file)
{
  hadesmem::ImportDirList import_dirs(process, pe_file);

  if (std::begin(import_dirs) != std::end(import_dirs))
  {
    std::wcout << "\n\tImport Dirs:\n";
  }
  else
  {
    std::wcout << "\n\tWARNING! Empty or invalid import directory.\n";
    WarnForCurrentFile(WarningType::kSuspicious);
  }

  std::uint32_t num_import_dirs = 0U;
  for (auto const& dir : import_dirs)
  {
    std::wcout << "\n";

    if (dir.IsTlsAoiTerminated())
    {
      std::wcout << "\t\tWARNING! Detected TLS AOI trick! Assuming a Windows 7 "
                    "style loader and terminating the list early.\n";
      WarnForCurrentFile(WarningType::kSuspicious);
      break;
    }

    DWORD const iat = dir.GetFirstThunk();
    bool const iat_valid = !!hadesmem::RvaToVa(process, pe_file, iat);
    DWORD const ilt = dir.GetOriginalFirstThunk();
    bool const use_ilt = !!ilt && ilt != iat;
    hadesmem::ImportThunkList ilt_thunks(process, pe_file, use_ilt ? ilt : iat);
    bool const ilt_empty = std::begin(ilt_thunks) == std::end(ilt_thunks);
    bool const ilt_valid = !!hadesmem::RvaToVa(process, pe_file, ilt);
    // TODO: Is it possible to have an empty but valid ILT? Woulnd't that mean
    // the IAT would also need to be empty? Is that (ILT with no thunks) even
    // allowed? IAT with no thunks means it's skipped...

    {
      // If the IAT is empty then the descriptor is skipped, and the name can
      // be invalid because it's ignored. Note that we simply skip here rather
      // than terminate, because it's possible to have such 'invalid' entries
      // in-between real entries.
      hadesmem::ImportThunkList iat_thunks(process, pe_file, iat);
      if (std::begin(iat_thunks) == std::end(iat_thunks))
      {
        std::wcout << "\t\tWARNING! IAT is "
                   << (iat_valid ? "empty" : "invalid") << ".\n";
        WarnForCurrentFile(WarningType::kSuspicious);
        continue;
      }
    }

    // TODO: Come up with a better solution to this.
    if (num_import_dirs++ == 1000)
    {
      std::wcout << "\t\tWARNING! Processed 1000 import dirs. Stopping early "
                    "to avoid resource exhaustion attacks. Check PE file for "
                    "TLS AOI trick, virtual terminator trick, or other similar "
                    "attacks.\n";
      WarnForCurrentFile(WarningType::kUnsupported);
      break;
    }

    std::wcout << "\t\tOriginalFirstThunk: " << std::hex
               << dir.GetOriginalFirstThunk() << std::dec << "\n";
    DWORD const time_date_stamp = dir.GetTimeDateStamp();
    std::wcout << "\t\tTimeDateStamp: " << std::hex << time_date_stamp
               << std::dec << "\n";
    if (time_date_stamp == static_cast<DWORD>(-1))
    {
      // Don't just check whether the ILT is invalid, but also ensure that
      // there's a valid bound import dir. In the case where the bound import
      // dir is invalid we just treat the IAT as the ILT on disk. See
      // dllmaxvals.dll for a PE file which has TimeDateStamp of 0xFFFFFFFF, no
      // ILT, and no bound import dir.
      // TODO: Is this allowed? I guess this is legal in the case where you
      // bind a DLL that doesn't have an ILT to begin wtih, at which point it
      // won't load if the bindings don't match, but we need to confirm this.
      // Warn so we can find samples for further investigation.
      if (!ilt_valid && HasValidNonEmptyBoundImportDescList(process, pe_file))
      {
        std::wcout << "\t\tWARNING! Detected new style bound imports with an "
                      "invalid ILT. Currently unhandled.\n";
        WarnForCurrentFile(WarningType::kUnsupported);
      }
    }
    DWORD const forwarder_chain = dir.GetForwarderChain();
    std::wcout << "\t\tForwarderChain: " << std::hex << forwarder_chain
               << std::dec << "\n";
    if (forwarder_chain != 0 && forwarder_chain != static_cast<DWORD>(-1))
    {
      std::wcout << "\t\tWARNING! Detected old style forwarder chain. "
                    "Currently unhandled.\n";
      WarnForCurrentFile(WarningType::kUnsupported);
    }
    std::wcout << "\t\tName (Raw): " << std::hex << dir.GetNameRaw() << std::dec
               << "\n";
    try
    {
      // Import names don't need to consist of only printable characters, as
      // long as they are zero-terminated.
      // TODO: Find a solution to the above case, and perhaps use a vector<char>
      // instead of a string in the cases where the name isn't printable.
      // TODO: Detect and handle the case where the string is terminated
      // virtually. Currently we have a hacky workaround where we push an
      // extra zero onto the end of the buffer, so it should "just work" for
      // now... But we definitely need to fix this properly. Test virtually
      // terminated strings with maxsecxp.exe from the Corkami PE corpus.
      std::wcout << "\t\tName: " << dir.GetName().c_str() << "\n";
    }
    catch (std::exception const& /*e*/)
    {
      std::wcout << "\t\tWARNING! Failed to read name.\n";
      WarnForCurrentFile(WarningType::kSuspicious);
    }
    std::wcout << "\t\tFirstThunk: " << std::hex << dir.GetFirstThunk()
               << std::dec << "\n";

    // TODO: Parse the IAT and ILT in parallel, in order to easily detect when
    // imports are bound in-memory. This will also mean we no longer need to
    // count the length of the ILT in order to terminate the IAT pass early.

    if (ilt_empty)
    {
      // Has to be the ILT if we get here because we did a check for an
      // empty/invalid IAT earlier on.
      std::wcout << "\n\t\tWARNING! ILT is "
                 << (ilt_valid ? "empty" : "invalid") << ".\n";

      WarnForCurrentFile(WarningType::kSuspicious);
    }
    else
    {
      std::wcout << "\n\t\tImport Thunks (" << (use_ilt ? "ILT" : "IAT")
                 << "):\n";
    }

    // TODO: Distinguish between new and old binding styles and handle
    // appropriately.
    // TODO: Detect when the import dir says it is bound with the new style, but
    // the file does not have a valid bound import dir. In this case it seems to
    // be ignored by the loader. We should warn for this, but we probably can't
    // change the way we interpret the data, because just because there's no
    // bound import dir doesn't mean the IAT contains legitimate un-bound data,
    // it could just be complete garbage. Need to confirm this though...
    bool const is_bound = !!dir.GetTimeDateStamp();
    // Assume that any PE files mapped as images in memory have had their
    // imports resolved.
    bool const is_memory_bound =
      (pe_file.GetType() == hadesmem::PeFileType::Image) && !use_ilt;
    bool const is_ilt_bound = (is_bound && !use_ilt) || is_memory_bound;
    bool const is_iat_bound =
      is_bound || (pe_file.GetType() == hadesmem::PeFileType::Image);
    std::size_t count = 0U;
    for (auto const& thunk : ilt_thunks)
    {
      // TODO: Come up with a better solution to this.
      if (count++ == 1000)
      {
        std::wcout << "\n\t\t\tWARNING! Processed 1000 import thunks. Stopping "
                      "early to avoid resource exhaustion attacks. Check PE "
                      "file for TLS AOI trick, virtual terminator trick, or "
                      "other similar attacks.\n";
        WarnForCurrentFile(WarningType::kUnsupported);
        break;
      }

      // TODO: Should probably revert to using 'is_ilt_bound' instead of
      // hardcoding false, but is it even legal to have a module that uses old
      // style bindings with no ILT? Need to investigate, because it seems
      // you're allowed to have modules like that when they're not actually
      // bound, and the loader simply detects that the TimeDateStamp doesn't
      // match and so treats the IAT as unbound? Investigate this further.
      (void)is_ilt_bound;
      DumpImportThunk(thunk, false);
    }

    // Windows will load PE files that have an invalid RVA for the ILT (lies
    // outside of the virtual space), and will fall back to the IAT in this
    // case.
    if (use_ilt && iat)
    {
      hadesmem::ImportThunkList iat_thunks(
        process, pe_file, dir.GetFirstThunk());
      if (std::begin(iat_thunks) != std::end(iat_thunks))
      {
        std::wcout << "\n\t\tImport Thunks (IAT):\n";
      }
      for (auto const& thunk : iat_thunks)
      {
        if (ilt_valid && !count--)
        {
          std::wcout << "\n\t\t\tWARNING! IAT size does not match ILT size. "
                        "Stopping IAT enumeration early.\n";
          WarnForCurrentFile(WarningType::kSuspicious);
          break;
        }

        // If the ILT is not empty (empty includes invalid) we simply treat the
        // IAT as bound, regardless of whether it actually is. This is because
        // apparently as long as you have a valid ILT you can put whatever the
        // hell you want in the IAT, because it's going to be overwitten anyway.
        // See tinynet.exe from the Corkami PE corpus for an example.
        // Furthermore, we only treat the IAT as bound if the ILT is also valid.
        // Not sure if this is correct, but apparently it's possible to have a
        // module with the TimeDateStamp set, indicating that the module is
        // bound, even though it actually isn't (and XP will apparently load
        // such a module). See tinygui.exe from the Corkami PE corpus for an
        // example.
        // TODO: Confirm this is correct.
        DumpImportThunk(thunk, (is_iat_bound && ilt_valid) || !ilt_empty);
      }
    }
  }
}

void DumpBoundImports(hadesmem::Process const& process,
                      hadesmem::PeFile const& pe_file)
{
  // TODO: Add similar checks elsewhere to reduce unnecessary warnings?
  if (!HasBoundImportDir(process, pe_file))
  {
    return;
  }

  // TODO: Only attempt to read the bound import dir if the module actually has
  // bound imports? Is this necessary anymore nwo that we've worked around the
  // RVA resolution problem?

  hadesmem::BoundImportDirList bound_import_dirs(process, pe_file);

  if (std::begin(bound_import_dirs) != std::end(bound_import_dirs))
  {
    std::wcout << "\n\tBound Import Dirs:\n";
  }
  else
  {
    std::wcout << "\n\tWARNING! Empty or invalid bound import directory.\n";
    WarnForCurrentFile(WarningType::kSuspicious);
  }

  for (auto const& dir : bound_import_dirs)
  {
    std::wcout << "\n";

    std::wcout << "\t\tTimeDateStamp: " << std::hex << dir.GetTimeDateStamp()
               << std::dec << "\n";
    std::wcout << "\t\tOffsetModuleName: " << std::hex
               << dir.GetOffsetModuleName() << std::dec << "\n";
    std::wcout << "\t\tModuleName: " << dir.GetModuleName().c_str() << "\n";
    std::wcout << "\t\tNumberOfModuleForwarderRefs: " << std::hex
               << dir.GetNumberOfModuleForwarderRefs() << std::dec << "\n";
    auto const forwarder_refs = dir.GetModuleForwarderRefs();
    if (std::begin(forwarder_refs) != std::end(forwarder_refs))
    {
      std::wcout << "\t\tModule Forwarder Refs:\n";
    }
    for (auto const& forwarder : forwarder_refs)
    {
      std::wcout << "\t\t\tTimeDateStamp: " << std::hex
                 << forwarder.TimeDateStamp << std::dec << "\n";
      std::wcout << "\t\t\tOffsetModuleName: " << std::hex
                 << forwarder.OffsetModuleName << std::dec << "\n";
      std::wcout << "\t\t\tModuleName: "
                 << dir.GetNameForModuleForwarderRef(forwarder).c_str() << "\n";
      std::wcout << "\t\t\tReserved: " << std::hex << forwarder.Reserved
                 << std::dec << "\n";
    }
  }
}
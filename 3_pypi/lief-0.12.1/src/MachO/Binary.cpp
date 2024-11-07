/* Copyright 2017 - 2022 R. Thomas
 * Copyright 2017 - 2022 Quarkslab
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <algorithm>
#include <numeric>
#include <sstream>

#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#include <unistd.h>
#else
#define getpagesize() 0x1000
#endif

#include "logging.hpp"


#include "Object.tcc"
#include "Binary.tcc"

#include "LIEF/utils.hpp"
#include "LIEF/BinaryStream/VectorStream.hpp"
#include "LIEF/BinaryStream/SpanStream.hpp"

#include "LIEF/MachO/hash.hpp"
#include "LIEF/MachO/Binary.hpp"
#include "LIEF/MachO/Builder.hpp"
#include "LIEF/MachO/SegmentCommand.hpp"
#include "LIEF/MachO/MainCommand.hpp"
#include "LIEF/MachO/ThreadCommand.hpp"
#include "LIEF/MachO/Symbol.hpp"
#include "LIEF/MachO/SymbolCommand.hpp"
#include "LIEF/MachO/SegmentSplitInfo.hpp"
#include "LIEF/MachO/DylibCommand.hpp"
#include "LIEF/MachO/Section.hpp"
#include "LIEF/MachO/Relocation.hpp"
#include "LIEF/MachO/DataInCode.hpp"
#include "LIEF/MachO/CodeSignature.hpp"
#include "LIEF/MachO/FunctionStarts.hpp"
#include "LIEF/MachO/DynamicSymbolCommand.hpp"
#include "LIEF/MachO/DyldInfo.hpp"
#include "LIEF/MachO/ExportInfo.hpp"
#include "LIEF/MachO/BindingInfo.hpp"
#include "LIEF/MachO/EnumToString.hpp"
#include "LIEF/MachO/DylinkerCommand.hpp"
#include "LIEF/MachO/UUIDCommand.hpp"
#include "LIEF/MachO/VersionMin.hpp"
#include "LIEF/MachO/RPathCommand.hpp"
#include "LIEF/MachO/SubFramework.hpp"
#include "LIEF/MachO/DyldEnvironment.hpp"
#include "LIEF/MachO/EncryptionInfo.hpp"
#include "LIEF/MachO/SourceVersion.hpp"
#include "MachO/Structures.hpp"

#include "LIEF/exception.hpp"

namespace LIEF {
namespace MachO {

bool Binary::KeyCmp::operator() (const Relocation* lhs, const Relocation* rhs) const {
  return *lhs < *rhs;
}

Binary::Binary() {
  format_ = LIEF::EXE_FORMATS::FORMAT_MACHO;
}

LIEF::Binary::sections_t Binary::get_abstract_sections() {
  LIEF::Binary::sections_t result;
  it_sections sections = this->sections();
  std::transform(std::begin(sections), std::end(sections),
                 std::back_inserter(result),
                 [] (Section& s) { return &s; });

  return result;
}
// LIEF Interface
// ==============

void Binary::patch_address(uint64_t address, const std::vector<uint8_t>& patch_value, LIEF::Binary::VA_TYPES) {
  // Find the segment associated with the virtual address
  SegmentCommand* segment_topatch = segment_from_virtual_address(address);
  if (segment_topatch == nullptr) {
    LIEF_ERR("Unable to find segment associated with address: 0x{:x}", address);
    return;
  }
  const uint64_t offset = address - segment_topatch->virtual_address();
  span<uint8_t> content = segment_topatch->writable_content();
  if (offset > content.size() || (offset + patch_value.size()) > content.size()) {
    LIEF_ERR("The patch value ({} bytes @0x{:x}) is out of bounds of the segment (limit: 0x{:x})",
             patch_value.size(), offset, content.size());
    return;
  }
  std::move(std::begin(patch_value), std::end(patch_value), content.data() + offset);
}

void Binary::patch_address(uint64_t address, uint64_t patch_value, size_t size, LIEF::Binary::VA_TYPES) {
  if (size > sizeof(patch_value)) {
    LIEF_ERR("Invalid size: 0x{:x}", size);
    return;
  }

  SegmentCommand* segment_topatch = segment_from_virtual_address(address);

  if (segment_topatch == nullptr) {
    LIEF_ERR("Unable to find segment associated with address: 0x{:x}", address);
    return;
  }
  const uint64_t offset = address - segment_topatch->virtual_address();
  span<uint8_t> content = segment_topatch->writable_content();

  if (offset > content.size() || (offset + size) > content.size()) {
    LIEF_ERR("The patch value ({} bytes @0x{:x}) is out of bounds of the segment (limit: 0x{:x})",
             size, offset, content.size());
    return;
  }

  switch (size) {
    case sizeof(uint8_t):
      {
        auto X = static_cast<uint8_t>(patch_value);
        memcpy(content.data() + offset, &X, sizeof(uint8_t));
        break;
      }

    case sizeof(uint16_t):
      {
        auto X = static_cast<uint16_t>(patch_value);
        memcpy(content.data() + offset, &X, sizeof(uint16_t));
        break;
      }

    case sizeof(uint32_t):
      {
        auto X = static_cast<uint32_t>(patch_value);
        memcpy(content.data() + offset, &X, sizeof(uint32_t));
        break;
      }

    case sizeof(uint64_t):
      {
        auto X = static_cast<uint64_t>(patch_value);
        memcpy(content.data() + offset, &X, sizeof(uint64_t));
        break;
      }

    default:
      {
        LIEF_ERR("The provided size ({}) does not match the size of an integer", size);
        return;
      }
  }


}

std::vector<uint8_t> Binary::get_content_from_virtual_address(uint64_t virtual_address, uint64_t size, LIEF::Binary::VA_TYPES) const {
  const SegmentCommand* segment = segment_from_virtual_address(virtual_address);

  if (segment == nullptr) {
    LIEF_ERR("Unable to find segment associated with address: 0x{:x}", virtual_address);
    return {};
  }

  span<const uint8_t> content = segment->content();
  const uint64_t offset = virtual_address - segment->virtual_address();

  uint64_t checked_size = size;
  if (offset > content.size() || (offset + checked_size) > content.size()) {
    checked_size = checked_size - (offset + checked_size - content.size());
  }

  return {content.data() + offset, content.data() + offset + checked_size};
}


uint64_t Binary::entrypoint() const {

  if (!has_entrypoint()) {
    throw not_found("Entrypoint not found");
  }

  if (const MainCommand* cmd = main_command()) {
    return imagebase() + cmd->entrypoint();
  }

  if (const ThreadCommand* cmd = thread_command()) {
    return imagebase() + cmd->pc();
  }

  throw not_found("Entrypoint not found");
}

bool Binary::is_pie() const {
  return header().has(HEADER_FLAGS::MH_PIE);
}


bool Binary::has_nx() const {
  if (!header().has(HEADER_FLAGS::MH_NO_HEAP_EXECUTION)) {
    LIEF_INFO("Heap could be executable");
  }
  return !header().has(HEADER_FLAGS::MH_ALLOW_STACK_EXECUTION);
}


bool Binary::has_entrypoint() const {
  return has_main_command() || has_thread_command();
}

LIEF::Binary::symbols_t Binary::get_abstract_symbols() {
  LIEF::Binary::symbols_t syms;
  syms.reserve(symbols_.size());
  std::transform(std::begin(symbols_), std::end(symbols_),
                 std::back_inserter(syms),
                 [] (const std::unique_ptr<Symbol>& s) {
                   return s.get();
                 });
  return syms;
}


LIEF::Binary::functions_t Binary::get_abstract_exported_functions() const {
  LIEF::Binary::functions_t result;
  it_const_exported_symbols syms = exported_symbols();
  std::transform(std::begin(syms), std::end(syms),
                 std::back_inserter(result),
                 [] (const Symbol& s) {
                   return Function{s.name(), s.value(),
                                   Function::flags_list_t{Function::FLAGS::EXPORTED}};
                 });
  return result;
}

LIEF::Binary::functions_t Binary::get_abstract_imported_functions() const {
  LIEF::Binary::functions_t result;
  it_const_imported_symbols syms = imported_symbols();
  std::transform(std::begin(syms), std::end(syms),
                 std::back_inserter(result),
                 [] (const Symbol& s) {
                   return Function{s.name(), s.value(),
                                   Function::flags_list_t{Function::FLAGS::IMPORTED}};
                 });
  return result;
}


std::vector<std::string> Binary::get_abstract_imported_libraries() const {
  std::vector<std::string> result;
  for (const DylibCommand& lib : libraries()) {
    result.push_back(lib.name());
  }
  return result;
}


const Header& Binary::header() const {
  return header_;
}

Header& Binary::header() {
  return const_cast<Header&>(static_cast<const Binary*>(this)->header());
}

// Commands
// ========

 Binary::it_commands Binary::commands() {
  return commands_;
}

Binary::it_const_commands Binary::commands() const {
  return commands_;
}


// Filesets
// ========

Binary::it_fileset_binaries Binary::filesets() {
  return filesets_;

}

Binary::it_const_fileset_binaries Binary::filesets() const {
  return filesets_;
}


// Symbols
// =======

Binary::it_symbols Binary::symbols() {
  return symbols_;
}

Binary::it_const_symbols Binary::symbols() const {
  return symbols_;
}

Binary::it_libraries Binary::libraries() {
  return libraries_;
}

Binary::it_const_libraries Binary::libraries() const {
  return libraries_;
}

Binary::it_segments Binary::segments() {
  return segments_;
}

Binary::it_const_segments Binary::segments() const {
  return segments_;
}

Binary::it_sections Binary::sections() {
  return sections_;
}

Binary::it_const_sections Binary::sections() const {
  return sections_;
}


// Relocations
Binary::it_relocations Binary::relocations() {
  relocations_t result;
  for (SegmentCommand* segment : segments_) {
    std::transform(std::begin(segment->relocations_), std::end(segment->relocations_),
                   std::inserter(result, std::begin(result)),
                   [] (const std::unique_ptr<Relocation>& r) {
                     return r.get();
                   });
  }

  for (Section* section : sections_) {
    std::transform(std::begin(section->relocations_), std::end(section->relocations_),
                   std::inserter(result, std::begin(result)),
                   [] (const std::unique_ptr<Relocation>& r) {
                     return r.get();
                   });
  }

  relocations_ = std::move(result);
  return relocations_;
}

Binary::it_const_relocations Binary::relocations() const {
  relocations_t result;
  for (const SegmentCommand* segment : segments_) {
    std::transform(std::begin(segment->relocations_), std::end(segment->relocations_),
                   std::inserter(result, std::begin(result)),
                   [] (const std::unique_ptr<Relocation>& r) {
                     return r.get();
                   });
  }

  for (const Section* section : sections_) {
    std::transform(std::begin(section->relocations_), std::end(section->relocations_),
                   std::inserter(result, std::begin(result)),
                   [] (const std::unique_ptr<Relocation>& r) {
                     return r.get();
                   });
  }

  relocations_ = std::move(result);
  return relocations_;
}


LIEF::Binary::relocations_t Binary::get_abstract_relocations() {
  LIEF::Binary::relocations_t relocations;
  it_relocations macho_relocations = this->relocations();
  relocations.reserve(macho_relocations.size());

  for (Relocation& r : macho_relocations) {
    relocations.push_back(&r);
  }

  return relocations;
}


// Symbols
// =======

bool Binary::is_exported(const Symbol& symbol) {
  return !symbol.is_external() && symbol.has_export_info();
}

Binary::it_exported_symbols Binary::exported_symbols() {
  return {symbols_, [] (const std::unique_ptr<Symbol>& symbol) {
    return is_exported(*symbol); }
  };
}

Binary::it_const_exported_symbols Binary::exported_symbols() const {
  return {symbols_, [] (const std::unique_ptr<Symbol>& symbol) {
    return is_exported(*symbol);
  }};
}

bool Binary::is_imported(const Symbol& symbol) {
  return symbol.is_external() && !symbol.has_export_info();
}

Binary::it_imported_symbols Binary::imported_symbols() {
  return {symbols_, [] (const std::unique_ptr<Symbol>& symbol) {
    return is_imported(*symbol);
  }};
}


Binary::it_const_imported_symbols Binary::imported_symbols() const {
  return {symbols_, [] (const std::unique_ptr<Symbol>& symbol) {
    return is_imported(*symbol); }
  };
}


bool Binary::has_symbol(const std::string& name) const {
  return get_symbol(name) != nullptr;
}

const Symbol* Binary::get_symbol(const std::string& name) const {
  const auto it_symbol = std::find_if(
      std::begin(symbols_), std::end(symbols_),
      [&name] (const std::unique_ptr<Symbol>& sym) {
        return sym->name() == name;
      });

  if (it_symbol == std::end(symbols_)) {
    return nullptr;
  }

  return it_symbol->get();
}

Symbol* Binary::get_symbol(const std::string& name) {
  return const_cast<Symbol*>(static_cast<const Binary*>(this)->get_symbol(name));
}

// =====


void Binary::write(const std::string& filename) {
  Builder::write(*this, filename);
}


const Section* Binary::section_from_offset(uint64_t offset) const {
  const auto it_section = std::find_if(
      sections_.cbegin(), sections_.cend(),
      [offset] (const Section* section) {
        return section->offset() <= offset &&
               offset < (section->offset() + section->size());
      });

  if (it_section == sections_.cend()) {
    return nullptr;
  }

  return *it_section;
}

Section* Binary::section_from_offset(uint64_t offset) {
  return const_cast<Section*>(static_cast<const Binary*>(this)->section_from_offset(offset));
}


const Section* Binary::section_from_virtual_address(uint64_t address) const {
  const auto it_section = std::find_if(
      std::begin(sections_), std::end(sections_),
      [address] (const Section* section) {
        return section->virtual_address() <= address &&
               address < (section->virtual_address() + section->size());
      });

  if (it_section == std::end(sections_)) {
    return nullptr;
  }

  return *it_section;
}

Section* Binary::section_from_virtual_address(uint64_t address) {
  return const_cast<Section*>(static_cast<const Binary*>(this)->section_from_virtual_address(address));
}

const SegmentCommand* Binary::segment_from_virtual_address(uint64_t virtual_address) const {
  auto it_segment = std::find_if(
      std::begin(segments_), std::end(segments_),
      [virtual_address] (const SegmentCommand* segment) {
        return segment->virtual_address() <= virtual_address &&
               virtual_address < (segment->virtual_address() + segment->virtual_size());
      });

  if (it_segment == std::end(segments_)) {
    return nullptr;
  }

  return *it_segment;
}

size_t Binary::segment_index(const SegmentCommand& segment) const {
  return segment.index();
}

SegmentCommand* Binary::segment_from_virtual_address(uint64_t virtual_address) {
  return const_cast<SegmentCommand*>(static_cast<const Binary*>(this)->segment_from_virtual_address(virtual_address));
}

const SegmentCommand* Binary::segment_from_offset(uint64_t offset) const {
  const auto it_begin = std::begin(offset_seg_);
  if (offset < it_begin->first) {
    return nullptr;
  }

  auto it = offset_seg_.lower_bound(offset);
  if (it->first == offset || it == it_begin) {
    return it->second;
  }

  const auto it_end = offset_seg_.crbegin();
  if (it == std::end(offset_seg_) && offset >= it_end->first) {
    return it_end->second;
  }
  --it;
  return it->second;
}

SegmentCommand* Binary::segment_from_offset(uint64_t offset) {
  return const_cast<SegmentCommand*>(static_cast<const Binary*>(this)->segment_from_offset(offset));
}

void Binary::shift_command(size_t width, size_t from_offset) {
  const SegmentCommand* segment = segment_from_offset(from_offset);

  size_t __text_base_addr = 0;
  size_t virtual_address = 0;

  if (segment != nullptr) {
    virtual_address = segment->virtual_address() + from_offset;
  }

  if (const SegmentCommand* text = get_segment("__TEXT")) {
    __text_base_addr = text->virtual_address();
  }


  // Shift symbols command
  // =====================
  if (SymbolCommand* sym_cmd = symbol_command()) {

    if (sym_cmd->symbol_offset() > from_offset) {
      sym_cmd->symbol_offset(sym_cmd->symbol_offset() + width);
    }

    if (sym_cmd->strings_offset() > from_offset) {
      sym_cmd->strings_offset(sym_cmd->strings_offset() + width);
    }

    for (std::unique_ptr<Symbol>& s : symbols_) {
      static constexpr size_t N_TYPE = 0x0e;
      if (static_cast<N_LIST_TYPES>(s->type() & N_TYPE) == N_LIST_TYPES::N_SECT) {
        uint64_t value = s->value();
        if (value > from_offset) {
          s->value(value + width);
        }
      }
    }
  }

  // Data In Code
  // ============
  if (DataInCode* data_code_cmd = data_in_code()) {
    if (data_code_cmd->data_offset() > from_offset) {
      data_code_cmd->data_offset(data_code_cmd->data_offset() + width);
    }
  }


  // Code Signature
  // ==============
  if (CodeSignature* sig = code_signature()) {
    if (sig->data_offset() > from_offset) {
      sig->data_offset(sig->data_offset() + width);
    }
  }

  if (CodeSignature* sig_dir = code_signature_dir()) {
    if (sig_dir->data_offset() > from_offset) {
      sig_dir->data_offset(sig_dir->data_offset() + width);
    }
  }

  if (SegmentSplitInfo* ssi = segment_split_info()) {
    if (ssi->data_offset() > from_offset) {
      ssi->data_offset(ssi->data_offset() + width);
    }
  }

  // Shift Main Command
  // ==================
  if (MainCommand* main_cmd = main_command()) {
     if ((__text_base_addr + main_cmd->entrypoint()) > virtual_address) {
      main_cmd->entrypoint(main_cmd->entrypoint() + width);
    }
  }

  // Patch function starts
  // =====================
  if (FunctionStarts* fs = function_starts()) {
    fs->data_offset(fs->data_offset() + width);
    for (uint64_t& address : fs->functions()) {
      if ((__text_base_addr + address) > virtual_address) {
        address += width;
      }
    }
  }

  // Dynamic symbol command
  // ======================
  if (DynamicSymbolCommand* dyn_cmd = dynamic_symbol_command()) {
    if (dyn_cmd->toc_offset() > from_offset) {
      dyn_cmd->toc_offset(dyn_cmd->toc_offset() + width);
    }

    if (dyn_cmd->module_table_offset() > from_offset) {
      dyn_cmd->module_table_offset(dyn_cmd->module_table_offset() + width);
    }

    if (dyn_cmd->external_reference_symbol_offset() > from_offset) {
      dyn_cmd->external_reference_symbol_offset(dyn_cmd->external_reference_symbol_offset() + width);
    }

    if (dyn_cmd->indirect_symbol_offset() > from_offset) {
      dyn_cmd->indirect_symbol_offset(dyn_cmd->indirect_symbol_offset() + width);
    }

    if (dyn_cmd->external_relocation_offset() > from_offset) {
      dyn_cmd->external_relocation_offset(dyn_cmd->external_relocation_offset() + width);
    }

    if (dyn_cmd->local_relocation_offset() > from_offset) {
      dyn_cmd->local_relocation_offset(dyn_cmd->local_relocation_offset() + width);
    }
  }

  // Patch Dyld
  // ==========
  if (DyldInfo* dyld = dyld_info()) {

    // Shift underlying containers offset
    if (dyld->rebase().first > from_offset) {
      dyld->set_rebase_offset(dyld->rebase().first + width);
    }

    if (dyld->bind().first > from_offset) {
      dyld->set_bind_offset(dyld->bind().first + width);
    }

    if (dyld->weak_bind().first > from_offset) {
      dyld->set_weak_bind_offset(dyld->weak_bind().first + width);
    }

    if (dyld->lazy_bind().first > from_offset) {
      dyld->set_lazy_bind_offset(dyld->lazy_bind().first + width);
    }

    if (dyld->export_info().first > from_offset) {
      dyld->set_export_offset(dyld->export_info().first + width);
    }


    // Shift Relocations
    // -----------------
    // TODO: Optimize this code
    for (Relocation& reloc : relocations()) {
      if (reloc.address() > virtual_address) {
        if (is64_) {
          patch_relocation<uint64_t>(reloc, /* from */ virtual_address, /* shift */ width);
        } else {
          patch_relocation<uint32_t>(reloc, /* from */ virtual_address, /* shift */ width);
        }
        reloc.address(reloc.address() + width);
      }
    }

    // Shift Export Info
    // -----------------
    for (ExportInfo& info : dyld->exports()) {
      if (info.address() > virtual_address) {
        info.address(info.address() + width);
      }
    }

    // Shift bindings
    // --------------
    for (BindingInfo& info : dyld->bindings()) {
      if (info.address() > virtual_address) {
        info.address(info.address() + width);
      }
    }
  }

}


void Binary::shift(size_t value) {
  Header& header = this->header();

  // Offset of the load commands table
  const uint64_t loadcommands_start = is64_ ? sizeof(details::mach_header_64) :
                                              sizeof(details::mach_header);

  // End offset of the load commands table
  const uint64_t loadcommands_end = loadcommands_start + header.sizeof_cmds();

  // Segment that wraps this load command table
  SegmentCommand* load_cmd_segment = segment_from_offset(loadcommands_end);
  if (load_cmd_segment == nullptr) {
    LIEF_WARN("Can't find segment associated with last load command");
    return;
  }
  LIEF_DEBUG("LC Table wrapped by {} / End offset: 0x{:x} (size: {:x})",
             load_cmd_segment->name(), loadcommands_end, load_cmd_segment->data_.size());
  load_cmd_segment->content_insert(loadcommands_end, value);

  // 1. Shift all commands
  // =====================
  for (std::unique_ptr<LoadCommand>& cmd : commands_) {
    if (cmd->command_offset() >= loadcommands_end) {
      cmd->command_offset(cmd->command_offset() + value);
    }
  }

  shift_command(value, loadcommands_end);

  // Shift Segment and sections
  // ==========================
  offset_seg_.clear();
  for (SegmentCommand* segment : segments_) {
    // Extend the virtual size of the segment containing our shift
    if (segment->file_offset() <= loadcommands_end &&
        loadcommands_end < (segment->file_offset() + segment->file_size()))
    {
      segment->virtual_size(segment->virtual_size() + value);
      segment->file_size(segment->file_size() + value);

      for (const std::unique_ptr<Section>& section : segment->sections_) {
        if (section->offset() >= loadcommands_end) {
          section->offset(section->offset() + value);
          section->virtual_address(section->virtual_address() + value);
        }
      }
    } else {
      if (segment->file_offset() >= loadcommands_end) {
        segment->file_offset(segment->file_offset() + value);
        segment->virtual_address(segment->virtual_address() + value);
      }

      for (const std::unique_ptr<Section>& section : segment->sections_) {
        if (section->offset() >= loadcommands_end) {
          section->offset(section->offset() + value);
          section->virtual_address(section->virtual_address() + value);
        }

        if (section->type() == MACHO_SECTION_TYPES::S_ZEROFILL) {
          section->virtual_address(section->virtual_address() + value);
        }
      }
    }

    offset_seg_[segment->file_offset()] = segment;
  }

}


LoadCommand& Binary::add(const LoadCommand& command) {
  static constexpr uint32_t shift_value = 0x10000;
  const int32_t size_aligned = align(command.size(), pointer_size());

  // Check there is enough spaces between the load command table
  // and the raw content
  if (available_command_space_ < size_aligned) {
    shift(shift_value);
    available_command_space_ += shift_value;
    return add(command);
  }

  available_command_space_ -= size_aligned;

  Header& header = this->header();

  // Get border of the load command table
  const uint64_t loadcommands_start = is64_ ? sizeof(details::mach_header_64) :
                                              sizeof(details::mach_header);
  const uint64_t loadcommands_end = loadcommands_start + header.sizeof_cmds();

  // Update the Header according to the command that will be added
  header.sizeof_cmds(header.sizeof_cmds() + size_aligned);
  header.nb_cmds(header.nb_cmds() + 1);

  // Get the segment handling the LC table
  SegmentCommand* load_cmd_segment = segment_from_offset(loadcommands_end);
  if (load_cmd_segment == nullptr) {
    LIEF_WARN("Can't get the last load command");
    throw not_found("Can't get the last load command");
  }

  span<const uint8_t> content_ref = load_cmd_segment->content();
  std::vector<uint8_t> content = {std::begin(content_ref), std::end(content_ref)};

  // Copy the command data
  std::copy(std::begin(command.data()), std::end(command.data()),
            std::begin(content) + loadcommands_end);

  load_cmd_segment->content(std::move(content));

  // Add the command in the Binary
  std::unique_ptr<LoadCommand> copy(command.clone());
  copy->command_offset(loadcommands_end);


  // Update cache
  if (DylibCommand::classof(copy.get())) {
    libraries_.push_back(copy->as<DylibCommand>());
  }

  if (SegmentCommand::classof(copy.get())) {
    add_cached_segment(*copy->as<SegmentCommand>());
  }

  commands_.push_back(std::move(copy));
  return *commands_.back();
}

LoadCommand& Binary::add(const LoadCommand& command, size_t index) {
  static constexpr uint32_t shift_value = 0x10000;

  // If index is "too" large <=> push_back
  if (index >= commands_.size()) {
    return add(command);
  }

  int32_t size_aligned = align(command.size(), pointer_size());

  // Check that we have enough space
  if (available_command_space_ < size_aligned) {
    shift(shift_value);
    available_command_space_ += shift_value;
    return add(command, index);
  }

  available_command_space_ -= size_aligned;


  // Update the Header according to the new command
  Header& header = this->header();

  header.sizeof_cmds(header.sizeof_cmds() + size_aligned);
  header.nb_cmds(header.nb_cmds() + 1);

  // Get offset of the LC border
  LoadCommand* cmd_border = commands_[index].get();
  size_t border_off = cmd_border->command_offset();

  std::unique_ptr<LoadCommand> copy{command.clone()};
  copy->command_offset(cmd_border->command_offset());

  // Patch LC offsets that follow the LC border
  for (std::unique_ptr<LoadCommand>& lc : commands_) {
    if (lc->command_offset() >= border_off) {
      lc->command_offset(lc->command_offset() + size_aligned);
    }
  }

  if (DylibCommand::classof(copy.get())) {
    libraries_.push_back(copy->as<DylibCommand>());
  }

  if (SegmentCommand::classof(copy.get())) {
    add_cached_segment(*copy->as<SegmentCommand>());
  }
  LoadCommand* copy_ptr = copy.get();
  commands_.insert(std::begin(commands_) + index, std::move(copy));
  return *copy_ptr;
}

bool Binary::remove(const LoadCommand& command) {

  const auto it = std::find_if(
      std::begin(commands_), std::end(commands_),
      [&command] (const std::unique_ptr<LoadCommand>& cmd) {
        return *cmd == command;
      });

  if (it == std::end(commands_)) {
    LIEF_ERR("Unable to find command: {}", command);
    return false;
  }

  LoadCommand* cmd_rm = it->get();

  if (DylibCommand::classof(cmd_rm)) {
    auto it_cache = std::find(std::begin(libraries_), std::end(libraries_), cmd_rm);
    if (it_cache == std::end(libraries_)) {
      const auto* lib = cmd_rm->as<const DylibCommand>();
      LIEF_WARN("Library {} not found in cache. The binary object is likely in an inconsistent state", lib->name());
    } else {
      libraries_.erase(it_cache);
    }
  }

  if (SegmentCommand::classof(cmd_rm)) {
    auto it_cache = std::find(std::begin(segments_), std::end(segments_), cmd_rm);
    const auto* seg = cmd_rm->as<const SegmentCommand>();
    if (it_cache == std::end(segments_)) {
      LIEF_WARN("Segment {} not found in cache. The binary object is likely in an inconsistent state", seg->name());
    } else {
      // Update the indexes to keep a consistent state
      for (auto it = it_cache; it != std::end(segments_); ++it) {
        (*it)->index_--;
      }
      segments_.erase(it_cache);
    }
    auto it_offset = offset_seg_.find(seg->file_offset());
    if (it_offset != std::end(offset_seg_)) {
      offset_seg_.erase(it_offset);
    }
  }

  const size_t cmd_rm_offset = cmd_rm->command_offset();
  for (std::unique_ptr<LoadCommand>& cmd : commands_) {
    if (cmd->command_offset() >= cmd_rm_offset) {
      cmd->command_offset(cmd->command_offset() - cmd_rm->size());
    }
  }


  Header& header = this->header();
  header.sizeof_cmds(header.sizeof_cmds() - cmd_rm->size());
  header.nb_cmds(header.nb_cmds() - 1);
  available_command_space_ += cmd_rm->size();

  commands_.erase(it);
  return true;
}


bool Binary::remove(LOAD_COMMAND_TYPES type) {
  bool removed = false;

  while (LoadCommand* cmd = get(type)) {
    removed = remove(*cmd);
  }
  return removed;
}

bool Binary::remove_command(size_t index) {
  if (index >= commands_.size()) {
    return false;
  }
  return remove(*commands_[index]);
}

bool Binary::has(LOAD_COMMAND_TYPES type) const {
  const auto it = std::find_if(
      std::begin(commands_), std::end(commands_),
      [type] (const std::unique_ptr<LoadCommand>& cmd) {
        return cmd->command() == type;
      });
  return it != std::end(commands_);
}

const LoadCommand* Binary::get(LOAD_COMMAND_TYPES type) const {
  const auto it = std::find_if(
      std::begin(commands_), std::end(commands_),
      [type] (const std::unique_ptr<LoadCommand>& cmd) {
        return cmd->command() == type;
      });

  if (it == std::end(commands_)) {
    return nullptr;
  }
  return it->get();
}

LoadCommand* Binary::get(LOAD_COMMAND_TYPES type) {
  return const_cast<LoadCommand*>(static_cast<const Binary*>(this)->get(type));
}

bool Binary::extend(const LoadCommand& command, uint64_t size) {
  static constexpr uint32_t shift_value = 0x10000;

  const auto it = std::find_if(
      std::begin(commands_), std::end(commands_),
      [&command] (const std::unique_ptr<LoadCommand>& cmd) {
        return *cmd == command;
      });

  if (it == std::end(commands_)) {
    LIEF_ERR("Unable to find command: {}", command);
    return false;
  }

  LoadCommand* cmd = it->get();
  const int32_t size_aligned = align(cmd->size() + size, pointer_size());
  const uint32_t extension = size_aligned - cmd->size();
  if (available_command_space_ < size_aligned) {
    shift(shift_value);
    available_command_space_ += shift_value;
    return extend(command, size);
  }

  for (std::unique_ptr<LoadCommand>& lc : commands_) {
    if (lc->command_offset() > cmd->command_offset()) {
      lc->command_offset(lc->command_offset() + extension);
    }
  }

  cmd->size(size_aligned);

  // Update Header
  // =============
  Header& header = this->header();
  header.sizeof_cmds(header.sizeof_cmds() + extension);

  return true;
}


bool Binary::extend_segment(const SegmentCommand& segment, size_t size) {

  const auto it_segment = std::find_if(
      std::begin(segments_), std::end(segments_),
      [&segment] (const SegmentCommand* s) {
        return segment == *s;
      });

  if (it_segment == std::end(segments_)) {
    LIEF_ERR("Unable to find segment: '{}'", segment.name());
    return false;
  }

  SegmentCommand* target_segment = *it_segment;
  const size_t last_offset = target_segment->file_offset() + target_segment->file_size();
  const size_t last_va     = target_segment->virtual_address() + target_segment->virtual_size();

  const int32_t size_aligned = align(size, pointer_size());

  shift_command(size_aligned, last_offset - 4);

  // Shift Segment and sections
  // ==========================
  for (SegmentCommand* segment : segments_) {
    if (segment->file_offset() >= last_offset) {
      segment->file_offset(segment->file_offset() + size_aligned);
      segment->virtual_address(segment->virtual_address() + size_aligned);
    }

    for (const std::unique_ptr<Section>& section : segment->sections_) {
      if (section->offset() >= last_offset) {
        section->offset(section->offset() + size_aligned);
        section->virtual_address(section->virtual_address() + size_aligned);
      }

      if (section->type() == MACHO_SECTION_TYPES::S_ZEROFILL &&
          section->virtual_address() > last_va)
      {
        section->virtual_address(section->virtual_address() + size_aligned);
      }
    }
  }


  target_segment->virtual_size(target_segment->virtual_size() + size_aligned);
  target_segment->file_size(target_segment->file_size() + size_aligned);
  target_segment->content_resize(target_segment->file_size());
  return true;
}

void Binary::remove_section(const std::string& name, bool clear) {
  Section* sec_to_delete = get_section(name);
  if (sec_to_delete == nullptr) {
    LIEF_ERR("Can't find section '{}'", name);
    return;
  }
  SegmentCommand* segment = sec_to_delete->segment();
  if (segment == nullptr) {
    LIEF_ERR("The section {} is in an inconsistent state (missing segment). Can't remove it",
             sec_to_delete->name());
    return;
  }

  if (clear) {
    sec_to_delete->clear(0);
  }

  segment->numberof_sections(segment->numberof_sections() - 1);
  auto it_section = std::find_if(
      std::begin(segment->sections_), std::end(segment->sections_),
      [sec_to_delete] (const std::unique_ptr<Section>& s) {
        return *s == *sec_to_delete;
      });

  if (it_section == std::end(segment->sections_)) {
    LIEF_WARN("Can't find the section");
    return;
  }

  const size_t lc_offset = segment->command_offset();
  const size_t section_struct_size = is64_ ? sizeof(details::section_64) :
                                             sizeof(details::section_32);
  segment->size_ -= section_struct_size;

  header().sizeof_cmds(header().sizeof_cmds() - section_struct_size);

  for (std::unique_ptr<LoadCommand>& lc : commands_) {
    if (lc->command_offset() > lc_offset) {
      lc->command_offset(lc->command_offset() - section_struct_size);
    }
  }

  available_command_space_ += section_struct_size;

  std::unique_ptr<Section>& section = *it_section;
  // Remove from cache
  auto it_cache = std::find_if(std::begin(sections_), std::end(sections_),
      [&section] (const Section* sec) {
        return section.get() == sec;
      });

  if (it_cache == std::end(sections_)) {
    LIEF_WARN("Can find the section {} in the cache. The binary object is likely in an inconsistent state",
              section->name());
  } else {
    sections_.erase(it_cache);
  }

  segment->sections_.erase(it_section);
}

Section* Binary::add_section(const Section& section) {
  SegmentCommand* _TEXT_segment = get_segment("__TEXT");
  if (_TEXT_segment == nullptr) {
    LIEF_ERR("Unable to get '__TEXT' segment");
    return nullptr;
  }
  return add_section(*_TEXT_segment, section);
}


Section* Binary::add_section(const SegmentCommand& segment, const Section& section) {

  const auto it_segment = std::find_if(
      std::begin(segments_), std::end(segments_),
      [&segment] (const SegmentCommand* s) {
        return segment == *s;
      });

  if (it_segment == std::end(segments_)) {
    LIEF_ERR("Unable to find segment: '{}'", segment.name());
    return nullptr;
  }
  SegmentCommand* target_segment = *it_segment;

  span<const uint8_t> content_ref = section.content();
  Section::content_t content = {std::begin(content_ref), std::end(content_ref)};

  const size_t sec_size = is64_ ? sizeof(details::section_64) :
                                  sizeof(details::section_32);
  const size_t data_size = content.size();
  const int32_t needed_size = align(sec_size + data_size, getpagesize());
  if (available_command_space_ < needed_size) {
    shift(needed_size);
    available_command_space_ += needed_size;
    return add_section(segment, section);
  }

  if (!extend(*target_segment, sec_size)) {
    LIEF_ERR("Unable to extend segment '{}' by 0x{:x}", segment.name(), sec_size);
    return nullptr;
  }

  available_command_space_ -= needed_size;

  auto new_section = std::make_unique<Section>(section);
  // Compute offset, virtual address etc for the new section
  // =======================================================

  // Section raw data will be located just after commands table
  if (section.offset() == 0) {
    uint64_t new_offset = is64_ ? sizeof(details::mach_header_64) :
                                  sizeof(details::mach_header);
    new_offset += header().sizeof_cmds();
    new_offset += available_command_space_;
    new_section->offset(new_offset);
  }

  if (section.size() == 0) {
    new_section->size(data_size);
  }

  if (section.virtual_address() == 0) {
    new_section->virtual_address(target_segment->virtual_address() + new_section->offset());
  }

  new_section->segment_ = target_segment;
  target_segment->numberof_sections(target_segment->numberof_sections() + 1);

  // Copy the new section in the cache
  sections_.push_back(new_section.get());

  // Copy data to segment
  const size_t relative_offset = new_section->offset() - target_segment->file_offset();

  std::move(std::begin(content), std::end(content),
            std::begin(target_segment->data_) + relative_offset);

  target_segment->sections_.push_back(std::move(new_section));
  return target_segment->sections_.back().get();
}


LoadCommand& Binary::add(const SegmentCommand& segment) {
  SegmentCommand new_segment = segment;

  range_t va_ranges  = this->va_ranges();


  if (new_segment.file_size() == 0) {
    const uint64_t new_size = segment.content().size();
    new_segment.file_size(new_size);
  }

  if (new_segment.virtual_size() == 0) {
    const uint64_t new_size = align(new_segment.file_size(), getpagesize());
    new_segment.virtual_size(new_size);
  }

  if (segment.sections().size() > 0) {
    new_segment.nb_sections_ = segment.sections().size();
  }

  if (is64_) {
    new_segment.command(LOAD_COMMAND_TYPES::LC_SEGMENT_64);
    size_t needed_size = sizeof(details::segment_command_64);
    needed_size += new_segment.numberof_sections() * sizeof(details::section_64);
    new_segment.size(needed_size);
  } else {
    new_segment.command(LOAD_COMMAND_TYPES::LC_SEGMENT);
    size_t needed_size = sizeof(details::segment_command_32);
    needed_size += new_segment.numberof_sections() * sizeof(details::section_32);
    new_segment.size(needed_size);
  }


  // Insert the segment before __LINKEDIT
  const auto it_linkedit = std::find_if(
      std::begin(commands_), std::end(commands_),
      [] (const std::unique_ptr<LoadCommand>& cmd) {
        if (!SegmentCommand::classof(cmd.get())) {
          return false;
        }
        return cmd->as<SegmentCommand>()->name() == "__LINKEDIT";
      });

  size_t pos = std::distance(std::begin(commands_), it_linkedit);
  auto& segment_added = *add(new_segment, pos).as<SegmentCommand>();

  // As virtual address should be shifted after "add" we need to re-update the virtual address after this operation
  range_t new_va_ranges  = this->va_ranges();
  range_t new_off_ranges = off_ranges();

  const bool should_patch = (new_va_ranges.second - segment_added.virtual_size()) != va_ranges.second;
  if (segment.virtual_address() == 0 && should_patch) {
    const uint64_t new_va = align(new_va_ranges.second, getpagesize());
    segment_added.virtual_address(new_va);
    size_t current_va = segment_added.virtual_address();
    for (Section& section : segment_added.sections()) {
      section.virtual_address(current_va);
      current_va += section.size();
    }

  }

  if (segment.file_offset() == 0 && should_patch) {
    const uint64_t new_offset = align(new_off_ranges.second, getpagesize());
    segment_added.file_offset(new_offset);
    size_t current_offset = new_offset;
    for (Section& section : segment_added.sections()) {
      section.offset(current_offset);

      current_offset += section.size();
    }
  }

  return segment_added;
}

size_t Binary::add_cached_segment(SegmentCommand& segment) {
  // The new segement should be put **befor** the __LINKEDIT
  // segment
  const auto it_linkedit = std::find_if(std::begin(segments_), std::end(segments_),
      [] (SegmentCommand* cmd) { return cmd->name() == "__LINKEDIT"; });

  if (it_linkedit == std::end(segments_)) {
    LIEF_DEBUG("No __LINKEDIT segment found!");
    segment.index_ = segments_.size();
    segments_.push_back(&segment);
  }
  segment.index_ = (*it_linkedit)->index();

  // Update indexes
  for (auto it = it_linkedit; it != std::end(segments_); ++it) {
    (*it)->index_++;
  }
  segments_.insert(it_linkedit, &segment);
  offset_seg_[segment.file_offset()] = &segment;
  segment.dyld_ = dyld_info();
  return segment.index();
}

bool Binary::unexport(const std::string& name) {
  const Symbol* s = get_symbol(name);
  if (s == nullptr) {
    return false;
  }
  return unexport(*s);
}

bool Binary::unexport(const Symbol& sym) {
  DyldInfo* dyld = dyld_info();
  if (dyld == nullptr) {
    LIEF_ERR("Can't find dyld info");
    return false;
  }
  const auto it_export = std::find_if(
      std::begin(dyld->export_info_), std::end(dyld->export_info_),
      [&sym] (const std::unique_ptr<ExportInfo>& info) {
        return info->has_symbol() && *info->symbol() == sym;
      });

  // The symbol is not exported
  if (it_export == std::end(dyld->export_info_)) {
    return false;
  }

  dyld->export_info_.erase(it_export);
  return true;
}

bool Binary::remove(const Symbol& sym) {
  unexport(sym);

  const auto it_symbol = std::find_if(
      std::begin(symbols_), std::end(symbols_),
      [&sym] (const std::unique_ptr<Symbol>& s) {
        return s->name() == sym.name();
      });

  // No Symbol
  if (it_symbol == std::end(symbols_)) {
    return false;
  }

  Symbol* symbol_to_remove = it_symbol->get();

  // Remove from the symbol command
  // ------------------------------
  SymbolCommand* sym_cmd = symbol_command();
  if (sym_cmd != nullptr) {
    if (sym_cmd->numberof_symbols() > 0) {
      sym_cmd->numberof_symbols(sym_cmd->numberof_symbols() - 1);
    }

    size_t size = is64_ ? sizeof(details::nlist_64) : sizeof(details::nlist_32);
    sym_cmd->strings_offset(sym_cmd->strings_offset() - size);
  }

  // Remove from the dynamic symbol command
  // --------------------------------------
  DynamicSymbolCommand* dynsym_cmd = dynamic_symbol_command();
  if (dynsym_cmd != nullptr) {
    std::vector<Symbol*> symtab;
    symtab.reserve(symbols_.size());
    for (std::unique_ptr<Symbol>& s : symbols_) {
      if (s->origin() == SYMBOL_ORIGINS::SYM_ORIGIN_LC_SYMTAB) {
        symtab.push_back(s.get());
      }
    }
    const auto it_symtab = std::find_if(
        std::begin(symtab), std::end(symtab),
        [symbol_to_remove] (const Symbol* symtab_sym) {
          return *symbol_to_remove == *symtab_sym;
        });

    if (it_symtab != std::end(symtab)) {
      size_t idx = std::distance(std::begin(symtab), it_symtab);

      // Update local symbols
      // ====================

      // Check if ``idx`` is included in
      // [idx_local_symbol, idx_local_symbol + nb_local_symbols [
      if (dynsym_cmd->idx_local_symbol() <= idx &&
          idx < (dynsym_cmd->idx_local_symbol() + dynsym_cmd->nb_local_symbols()))
      {
        dynsym_cmd->nb_local_symbols(dynsym_cmd->nb_local_symbols() - 1);

        if (idx == dynsym_cmd->idx_local_symbol()) {
          dynsym_cmd->idx_local_symbol(dynsym_cmd->idx_local_symbol() + 1);
        }
      }


      // External define symbols
      // =======================
      if (dynsym_cmd->idx_external_define_symbol() <= idx &&
          idx < (dynsym_cmd->idx_external_define_symbol() + dynsym_cmd->nb_external_define_symbols())) {
        dynsym_cmd->nb_external_define_symbols(dynsym_cmd->nb_external_define_symbols() - 1);
        if (idx == dynsym_cmd->idx_external_define_symbol()) {
          dynsym_cmd->idx_external_define_symbol(dynsym_cmd->idx_external_define_symbol() + 1);
        }
      }

      // Undefned symbols
      // ================
      if (dynsym_cmd->idx_undefined_symbol() <= idx &&
          idx < (dynsym_cmd->idx_undefined_symbol() + dynsym_cmd->nb_undefined_symbols())) {
        dynsym_cmd->nb_undefined_symbols(dynsym_cmd->nb_undefined_symbols() - 1);
        if (idx == dynsym_cmd->idx_undefined_symbol()) {
          dynsym_cmd->idx_undefined_symbol(dynsym_cmd->idx_undefined_symbol() + 1);
        }
      }

      if (idx < dynsym_cmd->idx_local_symbol()) {
        dynsym_cmd->idx_local_symbol(dynsym_cmd->idx_local_symbol() - 1);
      }

      if (idx < dynsym_cmd->idx_external_define_symbol()) {
        dynsym_cmd->idx_external_define_symbol(dynsym_cmd->idx_external_define_symbol() - 1);
      }

      if (idx < dynsym_cmd->idx_undefined_symbol()) {
        dynsym_cmd->idx_undefined_symbol(dynsym_cmd->idx_undefined_symbol() - 1);
      }

      //if (dynsym_cmd.nb_indirect_symbols() > 0) {
      //  dynsym_cmd.nb_indirect_symbols(dynsym_cmd.nb_indirect_symbols() - 1);
      //}
      // TODO: WIP
      // ==========================================
      if (dynsym_cmd->nb_indirect_symbols() > 0) {
        size_t size = is64_ ? sizeof(details::nlist_64) : sizeof(details::nlist_32);
        dynsym_cmd->indirect_symbol_offset(dynsym_cmd->indirect_symbol_offset() - size);
      }

      // ==================================
    }
  }


  // Remove from symbol table
  // ------------------------
  symbols_.erase(it_symbol);
  return true;
}

bool Binary::remove_symbol(const std::string& name) {
  bool removed = false;
  const Symbol* s = get_symbol(name);
  while (s != nullptr) {
    if (!remove(*s)) {
      break;
    }
    s = get_symbol(name);
    removed = true;
  }
  return removed;
}


bool Binary::can_remove(const Symbol& sym) const {
  // Check if binding are associated with this symbol

  const DyldInfo* dyld = dyld_info();
  if (dyld == nullptr) {
    return false;
  }

  DyldInfo::it_const_binding_info bindings = dyld->bindings();

  for (const BindingInfo& binding : bindings) {
    if (binding.has_symbol() && binding.symbol()->name() == sym.name()) {
      return false;
    }
  }
  return true;
}

bool Binary::can_remove_symbol(const std::string& name) const {
  std::vector<const Symbol*> syms;
  for (const std::unique_ptr<Symbol>& s : symbols_) {
    if (s->name() == name) {
      syms.push_back(s.get());
    }
  }
  return std::all_of(std::begin(syms), std::end(syms),
                     [this] (const Symbol* s) { return can_remove(*s); });
}


bool Binary::remove_signature() {
  const CodeSignature* cs = code_signature();
  if (cs == nullptr) {
    LIEF_WARN("No signature found");
    return false;
  }
  return remove(*cs);
}

LoadCommand& Binary::add(const DylibCommand& library) {
  return add(*library.as<LoadCommand>());
}

LoadCommand& Binary::add_library(const std::string& name) {
  return add(DylibCommand::load_dylib(name));
}

std::vector<uint8_t> Binary::raw() {
  Builder builder{*this};
  return builder.get_build();
}

uint64_t Binary::virtual_address_to_offset(uint64_t virtual_address) const {
  const SegmentCommand* segment = segment_from_virtual_address(virtual_address);
  if (segment == nullptr) {
    return static_cast<uint64_t>(-1);
  }
  const uint64_t base_address = segment->virtual_address() - segment->file_offset();
  return virtual_address - base_address;
}

uint64_t Binary::offset_to_virtual_address(uint64_t offset, uint64_t slide) const {
  const SegmentCommand* segment = segment_from_offset(offset);
  if (segment == nullptr) {
    return offset + slide;
  }
  const uint64_t base_address = segment->virtual_address() - segment->file_offset();
  if (slide > 0) {
    const uint64_t base = imagebase();
    if (base == 0) {
      return slide + offset;
    }
    return (base_address - base) + offset + slide;
  }
  return base_address + offset;
}

bool Binary::disable_pie() {
  if (is_pie()) {
    header().remove(HEADER_FLAGS::MH_PIE);
    return true;
  }
  return false;
}



bool Binary::has_section(const std::string& name) const {
  return get_section(name) != nullptr;
}

Section* Binary::get_section(const std::string& name) {
  return const_cast<Section*>(static_cast<const Binary*>(this)->get_section(name));
}

const Section* Binary::get_section(const std::string& name) const {
  const auto it_section = std::find_if(
      std::begin(sections_), std::end(sections_),
      [&name] (const Section* sec) {
        return sec->name() == name;
      });

  if (it_section == std::end(sections_)) {
    return nullptr;
  }

  return *it_section;
}


bool Binary::has_segment(const std::string& name) const {
  return get_segment(name) != nullptr;
}

const SegmentCommand* Binary::get_segment(const std::string& name) const {
  const auto it_segment = std::find_if(
      std::begin(segments_), std::end(segments_),
      [&name] (const SegmentCommand* seg) {
        return seg->name() == name;
      });

  if (it_segment == std::end(segments_)) {
    return nullptr;
  }

  return *it_segment;
}

SegmentCommand* Binary::get_segment(const std::string& name) {
  return const_cast<SegmentCommand*>(static_cast<const Binary*>(this)->get_segment(name));
}

uint64_t Binary::virtual_size() const {
  uint64_t virtual_size = 0;
  for (const SegmentCommand* segment : segments_) {
    virtual_size = std::max(virtual_size, segment->virtual_address() + segment->virtual_size());
  }
  virtual_size -= imagebase();
  virtual_size = align(virtual_size, static_cast<uint64_t>(getpagesize()));
  return virtual_size;
}

uint64_t Binary::imagebase() const {
  const SegmentCommand* _TEXT = get_segment("__TEXT");
  if (_TEXT == nullptr) {
    return 0;
  }
  return _TEXT->virtual_address();
}


std::string Binary::loader() const {
  const DylinkerCommand* cmd = dylinker();
  if (cmd == nullptr) {
    return "";
  }
  return cmd->name();
}

uint64_t Binary::fat_offset() const {
  return fat_offset_;
}


bool Binary::is_valid_addr(uint64_t address) const {
  range_t r = va_ranges();
  return r.first <= address && address < r.second;
}


Binary::range_t Binary::va_ranges() const {
  const auto it_min = std::min_element(
      std::begin(segments_), std::end(segments_),
      [] (const SegmentCommand* lhs, const SegmentCommand* rhs) {
        if (lhs->virtual_address() == 0 || rhs->virtual_address() == 0) {
          return true;
        }
        return lhs->virtual_address() < rhs->virtual_address();
      });

  const auto it_max = std::min_element(
      std::begin(segments_), std::end(segments_),
      [] (const SegmentCommand* lhs, const SegmentCommand* rhs) {
        return (lhs->virtual_address() + lhs->virtual_size()) > (rhs->virtual_address() + rhs->virtual_size());
      });

  return {(*it_min)->virtual_address(), (*it_max)->virtual_address() + (*it_max)->virtual_size()};
}

Binary::range_t Binary::off_ranges() const {

  const auto it_min = std::min_element(
      std::begin(segments_), std::end(segments_),
      [] (const SegmentCommand* lhs, const SegmentCommand* rhs) {
        if (lhs->file_offset() == 0 || rhs->file_offset() == 0) {
          return true;
        }
        return lhs->file_offset() < rhs->file_offset();
      });


  const auto it_max = std::min_element(
      std::begin(segments_), std::end(segments_),
      [] (const SegmentCommand* lhs, const SegmentCommand* rhs) {
        return (lhs->file_offset() + lhs->file_size()) > (rhs->file_offset() + rhs->file_size());
      });

  return {(*it_min)->file_offset(), (*it_max)->file_offset() + (*it_max)->file_size()};
}



LIEF::Header Binary::get_abstract_header() const {
  LIEF::Header header;
  const std::pair<ARCHITECTURES, std::set<MODES>>& am = this->header().abstract_architecture();
  header.architecture(am.first);
  header.modes(am.second);
  if (has_entrypoint()) {
    header.entrypoint(entrypoint());
  } else {
    header.entrypoint(0);
  }

  header.object_type(this->header().abstract_object_type());
  header.endianness(this->header().abstract_endianness());

  return header;
}


LIEF::Binary::functions_t Binary::ctor_functions() const {
  LIEF::Binary::functions_t functions;
  for (const Section& section : sections()) {
    if (section.type() != MACHO_SECTION_TYPES::S_MOD_INIT_FUNC_POINTERS) {
      continue;
    }

    span<const uint8_t> content = section.content();
    if (is64_) {
      const size_t nb_fnc = content.size() / sizeof(uint64_t);
      const auto* aptr = reinterpret_cast<const uint64_t*>(content.data());
      for (size_t i = 0; i < nb_fnc; ++i) {
        functions.emplace_back("ctor_" + std::to_string(i), aptr[i],
                               Function::flags_list_t{Function::FLAGS::CONSTRUCTOR});
      }

    } else {
      const size_t nb_fnc = content.size() / sizeof(uint32_t);
      const auto* aptr = reinterpret_cast<const uint32_t*>(content.data());
      for (size_t i = 0; i < nb_fnc; ++i) {
        functions.emplace_back("ctor_" + std::to_string(i), aptr[i],
                               Function::flags_list_t{Function::FLAGS::CONSTRUCTOR});
      }
    }
  }
  return functions;
}


LIEF::Binary::functions_t Binary::functions() const {
  static const auto func_cmd = [] (const Function& lhs, const Function& rhs) {
    return lhs.address() < rhs.address();
  };
  std::set<Function, decltype(func_cmd)> functions_set(func_cmd);

  LIEF::Binary::functions_t unwind_functions = this->unwind_functions();
  LIEF::Binary::functions_t ctor_functions   = this->ctor_functions();
  LIEF::Binary::functions_t exported         = get_abstract_exported_functions();

  std::move(std::begin(unwind_functions), std::end(unwind_functions),
            std::inserter(functions_set, std::end(functions_set)));

  std::move(std::begin(ctor_functions), std::end(ctor_functions),
            std::inserter(functions_set, std::end(functions_set)));

  std::move(std::begin(exported), std::end(exported),
            std::inserter(functions_set, std::end(functions_set)));

  return {std::begin(functions_set), std::end(functions_set)};

}

LIEF::Binary::functions_t Binary::unwind_functions() const {
  static constexpr size_t UNWIND_COMPRESSED = 3;
  static constexpr size_t UNWIND_UNCOMPRESSED = 2;

  // Set container to have functions with unique address
  static const auto fcmd = [] (const Function& l, const Function& r) {
    return l.address() < r.address();
  };
  std::set<Function, decltype(fcmd)> functions(fcmd);

  // Look for the __unwind_info section
  const Section* unwind_section = get_section("__unwind_info");
  if (unwind_section == nullptr) {
    return {};
  }

  SpanStream vs = unwind_section->content();

  // Get section content
  const auto hdr = vs.read<details::unwind_info_section_header>();
  if (!hdr) {
    LIEF_ERR("Can't read unwind section header!");
    return {};
  }
  vs.setpos(hdr->index_section_offset);

  size_t lsda_start = -1lu;
  size_t lsda_stop = 0;
  for (size_t i = 0; i < hdr->index_count; ++i) {
    const auto section_hdr = vs.read<details::unwind_info_section_header_index_entry>();
    if (!section_hdr) {
      LIEF_ERR("Can't read function information at index #{:d}", i);
      break;
    }

    functions.emplace(section_hdr->function_offset);
    const size_t second_lvl_off = section_hdr->second_level_pages_section_offset;
    const size_t lsda_off       = section_hdr->lsda_index_array_section_offset;

    lsda_start = std::min(lsda_off, lsda_start);
    lsda_stop  = std::max(lsda_off, lsda_stop);

    if (second_lvl_off > 0 && vs.can_read<details::unwind_info_regular_second_level_page_header>(second_lvl_off)) {
      const size_t saved_pos = vs.pos();
      {
        vs.setpos(second_lvl_off);
        const auto lvl_hdr = vs.peek<details::unwind_info_regular_second_level_page_header>(second_lvl_off);
        if (!lvl_hdr) {
          break;
        }
        if (lvl_hdr->kind == UNWIND_COMPRESSED) {
          const auto lvl_compressed_hdr = vs.read<details::unwind_info_compressed_second_level_page_header>();
          if (!lvl_compressed_hdr) {
            LIEF_ERR("Can't read lvl_compressed_hdr");
            break;
          }

          vs.setpos(second_lvl_off + lvl_compressed_hdr->entry_page_offset);
          for (size_t j = 0; j < lvl_compressed_hdr->entry_count; ++j) {
            auto entry = vs.read<uint32_t>();
            if (!entry) {
              break;
            }
            uint32_t func_off = section_hdr->function_offset + (*entry & 0xffffff);
            functions.emplace(func_off);
          }
        }
        else if (lvl_hdr->kind == UNWIND_UNCOMPRESSED) {
          LIEF_WARN("UNWIND_UNCOMPRESSED is not supported yet!");
        }
        else {
          LIEF_WARN("Unknown 2nd level kind: {:d}", lvl_hdr->kind);
        }
      }
      vs.setpos(saved_pos);
    }

  }

  const size_t nb_lsda = lsda_stop > lsda_start ? (lsda_stop - lsda_start) / sizeof(details::unwind_info_section_header_lsda_index_entry) : 0;
  vs.setpos(lsda_start);
  for (size_t i = 0; i < nb_lsda; ++i) {
    const auto hdr = vs.read<details::unwind_info_section_header_lsda_index_entry>();
    if (!hdr) {
      LIEF_ERR("Can't read LSDA at index #{:d}", i);
      break;
    }
    functions.emplace(hdr->function_offset);
  }

  return {std::begin(functions), std::end(functions)};
}

// UUID
// ++++
bool Binary::has_uuid() const {
  return has_command<UUIDCommand>();
}

UUIDCommand* Binary::uuid() {
  return command<UUIDCommand>();
}

const UUIDCommand* Binary::uuid() const {
  return command<UUIDCommand>();
}

// MainCommand
// +++++++++++
bool Binary::has_main_command() const {
  return has_command<MainCommand>();
}

MainCommand* Binary::main_command() {
  return command<MainCommand>();
}

const MainCommand* Binary::main_command() const {
  return command<MainCommand>();
}

// DylinkerCommand
// +++++++++++++++
bool Binary::has_dylinker() const {
  return has_command<DylinkerCommand>();
}

DylinkerCommand* Binary::dylinker() {
  return command<DylinkerCommand>();
}

const DylinkerCommand* Binary::dylinker() const {
  return command<DylinkerCommand>();
}

// DyldInfo
// ++++++++
bool Binary::has_dyld_info() const {
  return has_command<DyldInfo>();
}

DyldInfo* Binary::dyld_info() {
  return command<DyldInfo>();
}

const DyldInfo* Binary::dyld_info() const {
  return command<DyldInfo>();
}

// Function Starts
// +++++++++++++++
bool Binary::has_function_starts() const {
  return has_command<FunctionStarts>();
}

FunctionStarts* Binary::function_starts() {
  return command<FunctionStarts>();
}

const FunctionStarts* Binary::function_starts() const {
  return command<FunctionStarts>();
}

// Source Version
// ++++++++++++++
bool Binary::has_source_version() const {
  return has_command<SourceVersion>();
}

SourceVersion* Binary::source_version() {
  return command<SourceVersion>();
}

const SourceVersion* Binary::source_version() const {
  return command<SourceVersion>();
}

// Version Min
// +++++++++++
bool Binary::has_version_min() const {
  return has_command<VersionMin>();
}

VersionMin* Binary::version_min() {
  return command<VersionMin>();
}

const VersionMin* Binary::version_min() const {
  return command<VersionMin>();
}



// Thread command
// ++++++++++++++
bool Binary::has_thread_command() const {
  return has_command<ThreadCommand>();
}

ThreadCommand* Binary::thread_command() {
  return command<ThreadCommand>();
}

const ThreadCommand* Binary::thread_command() const {
  return command<ThreadCommand>();
}

// RPath command
// +++++++++++++
bool Binary::has_rpath() const {
  return has_command<RPathCommand>();
}

RPathCommand* Binary::rpath() {
  return command<RPathCommand>();
}

const RPathCommand* Binary::rpath() const {
  return command<RPathCommand>();
}

// SymbolCommand command
// +++++++++++++++++++++
bool Binary::has_symbol_command() const {
  return has_command<SymbolCommand>();
}

SymbolCommand* Binary::symbol_command() {
  return command<SymbolCommand>();
}

const SymbolCommand* Binary::symbol_command() const {
  return command<SymbolCommand>();
}

// DynamicSymbolCommand command
// ++++++++++++++++++++++++++++
bool Binary::has_dynamic_symbol_command() const {
  return has_command<DynamicSymbolCommand>();
}

DynamicSymbolCommand* Binary::dynamic_symbol_command() {
  return command<DynamicSymbolCommand>();
}

const DynamicSymbolCommand* Binary::dynamic_symbol_command() const {
  return command<DynamicSymbolCommand>();
}

// CodeSignature command
// +++++++++++++++++++++
bool Binary::has_code_signature() const {
  return has(LOAD_COMMAND_TYPES::LC_CODE_SIGNATURE);
}

CodeSignature* Binary::code_signature() {
  return const_cast<CodeSignature*>(static_cast<const Binary*>(this)->code_signature());
}

const CodeSignature* Binary::code_signature() const {
  if (const auto* cmd = get(LOAD_COMMAND_TYPES::LC_CODE_SIGNATURE)) {
    return cmd->as<const CodeSignature>();
  }
  return nullptr;
}


// CodeSignatureDir command
// ++++++++++++++++++++++++
bool Binary::has_code_signature_dir() const {
  return has(LOAD_COMMAND_TYPES::LC_DYLIB_CODE_SIGN_DRS);
}

CodeSignature* Binary::code_signature_dir() {
  return const_cast<CodeSignature*>(static_cast<const Binary*>(this)->code_signature_dir());
}

const CodeSignature* Binary::code_signature_dir() const {
  if (const auto* cmd = get(LOAD_COMMAND_TYPES::LC_DYLIB_CODE_SIGN_DRS)) {
    return cmd->as<const CodeSignature>();
  }
  return nullptr;
}


// DataInCode command
// ++++++++++++++++++
bool Binary::has_data_in_code() const {
  return has_command<DataInCode>();
}

DataInCode* Binary::data_in_code() {
  return command<DataInCode>();
}

const DataInCode* Binary::data_in_code() const {
  return command<DataInCode>();
}


// SegmentSplitInfo command
// ++++++++++++++++++++++++
bool Binary::has_segment_split_info() const {
  return has_command<SegmentSplitInfo>();
}

SegmentSplitInfo* Binary::segment_split_info() {
  return command<SegmentSplitInfo>();
}

const SegmentSplitInfo* Binary::segment_split_info() const {
  return command<SegmentSplitInfo>();
}


// SubFramework command
// ++++++++++++++++++++
bool Binary::has_sub_framework() const {
  return has_command<SubFramework>();
}

SubFramework* Binary::sub_framework() {
  return command<SubFramework>();
}

const SubFramework* Binary::sub_framework() const {
  return command<SubFramework>();
}

// DyldEnvironment command
// +++++++++++++++++++++++
bool Binary::has_dyld_environment() const {
  return has_command<DyldEnvironment>();
}

DyldEnvironment* Binary::dyld_environment() {
  return command<DyldEnvironment>();
}

const DyldEnvironment* Binary::dyld_environment() const {
  return command<DyldEnvironment>();
}

// EncryptionInfo command
// +++++++++++++++++++++++
bool Binary::has_encryption_info() const {
  return has_command<EncryptionInfo>();
}

EncryptionInfo* Binary::encryption_info() {
  return command<EncryptionInfo>();
}

const EncryptionInfo* Binary::encryption_info() const {
  return command<EncryptionInfo>();
}


// BuildVersion command
// ++++++++++++++++++++
bool Binary::has_build_version() const {
  return has_command<BuildVersion>();
}

BuildVersion* Binary::build_version() {
  return command<BuildVersion>();
}

const BuildVersion* Binary::build_version() const {
  return command<BuildVersion>();
}

bool Binary::has_filesets() const {
  return !filesets_.empty();
}

LoadCommand* Binary::operator[](LOAD_COMMAND_TYPES type) {
  return get(type);
}

const LoadCommand* Binary::operator[](LOAD_COMMAND_TYPES type) const {
  return get(type);
}


void Binary::accept(LIEF::Visitor& visitor) const {
  visitor.visit(*this);
}


Binary::~Binary() = default;

std::ostream& Binary::print(std::ostream& os) const {
  os << "Header" << std::endl;
  os << "======" << std::endl;

  os << header();
  os << std::endl;


  os << "Commands" << std::endl;
  os << "========" << std::endl;
  for (const LoadCommand& cmd : commands()) {
    os << cmd << std::endl;
  }

  os << std::endl;

  os << "Sections" << std::endl;
  os << "========" << std::endl;
  for (const Section& section : sections()) {
    os << section << std::endl;
  }

  os << std::endl;

  os << "Symbols" << std::endl;
  os << "=======" << std::endl;
  for (const Symbol& symbol : symbols()) {
    os << symbol << std::endl;
  }

  os << std::endl;
  return os;
}

}
}


#include "Main.h"

bool IS_ARM = false;

bool IsARM() {
    return std::string_view( "ARM" ) == inf.procname;
}

void AddByteToSignature( Signature& signature, ea_t address, bool wildcard ) {
    SignatureByte byte{};
    byte.isWildcard = wildcard;
    byte.value = get_byte( address );
    signature.push_back( byte );
}

void AddBytesToSignature( Signature& signature, ea_t address, size_t count, bool wildcard ) {
    signature.reserve( signature.size() + count );
    for( size_t i = 0; i < count; i++ ) {
        AddByteToSignature( signature, address + i, wildcard );
    }
}

bool GetOperandOffsetARM( const insn_t& instruction, uint8_t* operandOffset, uint8_t* operandLength ) {

    // Iterate all operands
    for( int i = 0; i < UA_MAXOP; i++ ) {
        auto& op = instruction.ops[i];

        // For ARM, we have to filter a bit though, only wildcard those operand types
        switch( op.type ) {
        case o_mem:
        case o_far:
        case o_near:
        case o_phrase:
        case o_displ:
        case o_imm:
            break;
        default:
            continue;
        }

        *operandOffset = op.offb;

        // This is somewhat of a hack because IDA api does not provide more info 
        // I always assume the operand is 3 bytes long with 1 byte operator
        if( instruction.size == 4 ) {
            *operandLength = 3;
        }
        // I saw some ADRL instruction having 8 bytes
        if( instruction.size == 8 ) {
            *operandLength = 7;
        }
        return true;
    }
    return false;
}

bool GetOperand( const insn_t& instruction, uint8_t* operandOffset, uint8_t* operandLength ) {

    // Handle ARM
    if( IS_ARM ) {
        return GetOperandOffsetARM( instruction, operandOffset, operandLength );
    }

    // Handle metapc x86/64

    // Iterate all operands
    for( int i = 0; i < UA_MAXOP; i++ ) {
        auto& op = instruction.ops[i];
        // Skip if we have no operand
        if( op.type == o_void ) {
            continue;
        }
        // offb = 0 means unknown
        if( op.offb == 0 ) {
            continue;
        }
        *operandOffset = op.offb;
        *operandLength = instruction.size - op.offb;
        return true;
    }
    return false;
}

bool IsSignatureUnique( std::string_view signature ) {
    // Convert signature string to searchable struct
    compiled_binpat_vec_t binaryPattern;
    parse_binpat_str( &binaryPattern, inf.min_ea, signature.data(), 16 );

    // Search for occurences
    auto lastOccurence = inf.min_ea;
    auto occurence = bin_search2( lastOccurence, inf.max_ea, binaryPattern, BIN_SEARCH_NOCASE | BIN_SEARCH_FORWARD );

    // Signature not found
    if( occurence == BADADDR ){
        return false;
    }

    // Check if it matches anywhere else
    lastOccurence = occurence + 1;
    occurence = bin_search2( lastOccurence, inf.max_ea, binaryPattern, BIN_SEARCH_NOCASE | BIN_SEARCH_FORWARD );

    // Signature matched only once
    return occurence == BADADDR;
}

// Signature to string 
std::string GenerateSignatureString( const Signature& signature, bool doubleQM = false ) {
    std::ostringstream result;
    // Build hex pattern
    for( const auto& byte : signature ) {
        if( byte.isWildcard ) {
            result << ( doubleQM ? "??" : "?" );
        }
        else {
            result << std::format( "{:02X}", byte.value );
        }
        result << " ";
    }
    auto str = result.str();
    // Remove whitespace at end
    if( !str.empty() ){
        str.pop_back();
    }
    return str;
}

std::string GenerateCodeSignatureString( const Signature& signature ) {
    std::ostringstream pattern;
    std::ostringstream mask;
    // Build hex pattern
    for( const auto& byte : signature ) {
        pattern << "\\x" << std::format( "{:02X}", ( byte.isWildcard ? 0 : byte.value ) );
        mask << ( byte.isWildcard ? "?" : "x" );
    }
    auto str = pattern.str() + " " + mask.str();
    return str;
}

std::string GenerateByteArrayWithBitMaskSignatureString( const Signature& signature ) {
    std::ostringstream pattern;
    std::ostringstream mask;
    // Build hex pattern
    for( const auto& byte : signature ) {
        pattern << "0x" << std::format( "{:02X}", ( byte.isWildcard ? 0 : byte.value ) ) << ", ";
        mask << ( byte.isWildcard ? "0" : "1" );
    }
    auto patternStr = pattern.str();
    auto maskStr = mask.str();

    // Reverse bitmask
    std::ranges::reverse( maskStr );

    // Remove separators
    if( !patternStr.empty() ) {
        patternStr.pop_back();
        patternStr.pop_back();
    }

    auto str = patternStr + " " + " 0b" + maskStr;
    return str;
}

bool SetClipboard( std::string_view text ) {
    if( text.empty() ) {
        msg( "[Error] Text empty" );
        return false;
    }

    if( OpenClipboard( NULL ) == false ) {
        msg( "[Error] Failed to open clipboard" );
        return false;
    }
    if( EmptyClipboard() == false ) {
        msg( "[Error] Failed to empty clipboard" );
    }

    auto memoryHandle = GlobalAlloc( GMEM_MOVEABLE | GMEM_ZEROINIT, text.size() + 1 );
    if( memoryHandle == nullptr ) {
        msg( "[Error] Failed to allocate clipboard memory" );
        CloseClipboard();
        return false;
    }

    auto textMem = reinterpret_cast< char* >( GlobalLock( memoryHandle ) );
    if( textMem == nullptr ) {
        msg( "[Error] Failed to lock clipboard memory" );
        GlobalFree( memoryHandle );
        CloseClipboard();
        return false;
    }

    memcpy( textMem, text.data(), text.size() );
    GlobalUnlock( memoryHandle );
    auto handle = SetClipboardData( CF_TEXT, memoryHandle );
    GlobalFree( memoryHandle );
    CloseClipboard();

    if( handle == nullptr ) {
        msg( "[Error] SetClipboardData failed" );
        return false;
    }
    return true;
}

// Trim wildcards at end
void TrimSignature( Signature& signature ) {
    auto it = std::find_if( signature.rbegin(), signature.rend(), []( const auto& sb ) { return !sb.isWildcard; } );
    signature.erase( it.base(), signature.end() );
}

std::optional<Signature> GenerateSignatureForEA( const ea_t ea, bool wildcardOperands, size_t maxSignatureLength = 1000, bool askLongerSignature = true ) {
    if( ea == BADADDR ) {
        msg( "Invalid address\n" );
        return std::nullopt;
    }

    if( !is_code( get_flags( ea ) ) ) {
        msg( "Can not create code signature for data\n" );
        return std::nullopt;
    }

    Signature signature;
    uint32_t sigPartLength = 0;

    auto currentAddress = ea;
    while( true ) {
        insn_t instruction;
        auto currentInstructionLength = decode_insn( &instruction, currentAddress );
        if( currentInstructionLength <= 0 ) {
            if( signature.empty() ) {
                msg( "Can't decode @ %I64X, is this actually code?\n", currentAddress );
                break;
            }

            msg( "Signature reached end of function @ %I64X\n", currentAddress );
            auto signatureString = GenerateSignatureString( signature );
            msg( "NOT UNIQUE Signature for %I64X: %s\n", ea, signatureString.c_str() );
            break;
        }

        // Length check in case the signature becomes too long
        if( sigPartLength > maxSignatureLength ) {
            if( askLongerSignature ) {
                auto result = ask_yn( 1, "Signature is already at %llu bytes. Continue?", signature.size() );
                if( result == 1 ) { // Yes 
                    sigPartLength = 0;
                }
                else if( result == 0 ) { // No
                    // Print the signature we have so far, even though its not unique
                    auto signatureString = GenerateSignatureString( signature );
                    msg( "NOT UNIQUE Signature for %I64X: %s\n", ea, signatureString.c_str() );
                    break;
                }
                else { // Cancel
                    break;
                }
            }
            else {
                return std::nullopt;
            }
        }
        sigPartLength += currentInstructionLength;

        uint8_t operandOffset = 0, operandLength = 0;
        if( wildcardOperands && GetOperand( instruction, &operandOffset, &operandLength ) && operandLength > 0 ) {
            // Add opcodes
            AddBytesToSignature( signature, currentAddress, operandOffset, false );
            // Wildcards for operands
            AddBytesToSignature( signature, currentAddress + operandOffset, operandLength, true );
            // If the operand is on the "left side", add the operator from the "right side"
            if( operandOffset == 0 ) {
                AddBytesToSignature( signature, currentAddress + operandLength, currentInstructionLength - operandLength, false );
            }
        }
        else {
            // No operand, add all bytes
            AddBytesToSignature( signature, currentAddress, currentInstructionLength, false );
        }

        auto currentSig = GenerateSignatureString( signature );
        if( IsSignatureUnique( currentSig ) ) {
            // Remove wildcards at end for output
            TrimSignature( signature );

            // Return the signature we generated
            return signature;
        }
        currentAddress += currentInstructionLength;
    }
    return std::nullopt;
}

std::string FormatSignature( const Signature& signature, SignatureType type ) {
    switch( type ) {
    case SignatureType::IDA:
        return GenerateSignatureString( signature );
    case SignatureType::x64Dbg:
        return GenerateSignatureString( signature, true );
    case SignatureType::Signature_Mask:
        return GenerateCodeSignatureString( signature );
    case SignatureType::SignatureByteArray_Bitmask:
        return GenerateByteArrayWithBitMaskSignatureString( signature );
    }
    return {};
}

void PrintSignatureForEA( const std::optional<Signature>& signature, ea_t ea, SignatureType sigType ) {
    if( !signature.has_value() ) {
        return;
    }
    const auto signatureStr = FormatSignature( signature.value(), sigType );
    msg( "Signature for %I64X: %s\n", ea, signatureStr.c_str() );
    SetClipboard( signatureStr );
}

void FindXRefs( ea_t ea, short wildcardOperands, std::vector<std::tuple<ea_t, Signature>>& xrefSignatures, size_t maxSignatureLength ) {
    xrefblk_t xref{};
    for( auto xref_ok = xref.first_to( ea, XREF_FAR ); xref_ok; xref_ok = xref.next_to() ) {

        // Skip data refs, xref.iscode is not what we want though
        if( !is_code( get_flags( xref.from ) ) ) {
            continue;
        }

        auto signature = GenerateSignatureForEA( xref.from, wildcardOperands, maxSignatureLength, false );
        if( !signature.has_value() ) {
            continue;
        }

        xrefSignatures.push_back( std::make_pair( xref.from, signature.value() ) );
    }

    // Sort signatures by length
    std::ranges::sort( xrefSignatures, []( const auto& a, const auto& b ) -> bool { return std::get<1>( a ).size() < std::get<1>( b ).size(); } );
}

void PrintXRefSignaturesForEA( ea_t ea, const std::vector<std::tuple<ea_t, Signature>>& xrefSignatures, SignatureType sigType, size_t topCount ) {
    if( xrefSignatures.empty() ) {
        msg( "No XREFs have been found for your address\n" );
        return;
    }

    // Print top 5 Signatures
    auto topLength = std::min( topCount, xrefSignatures.size() );
    msg( "Top %llu Signatures out of %llu xrefs for %I64X:\n", topLength, xrefSignatures.size(), ea );
    for( int i = 0; i < topLength; i++ ) {
        auto [originAddress, signature] = xrefSignatures[i];
        const auto signatureStr = FormatSignature( signature, sigType );
        msg( "XREF Signature #%i @ %I64X: %s\n", i + 1, originAddress, signatureStr.c_str() );

        // Copy first signature only
        if( i == 0 ) {
            SetClipboard( signatureStr );
        }
    }
}

void PrintSelectedCode( ea_t start, ea_t end, SignatureType sigType ) {
    auto selectionSize = end - start;
    if( selectionSize == 0 ) {
        msg( "Code selection %I64X-%I64X is too small!\n", start, end );
        return;
    }

    // Create signature from selection
    Signature signature;
    AddBytesToSignature( signature, start, selectionSize, false );
    const auto signatureStr = FormatSignature( signature, sigType );
    msg( "Code for %I64X-%I64X: %s\n", start, end, signatureStr.c_str() );
    SetClipboard( signatureStr );
}

bool idaapi plugin_ctx_t::run( size_t ) {

    // Check what processor we have
    if( IsARM() ) {
        IS_ARM = true;
    }

    // Show dialog
    const char format[] =
        "STARTITEM 0\n"                                                         // TabStop
        "Signature Maker\n"                                                     // Title

        "Select action:\n"                                                      // Title
        "<Create Signature for current code address:R>\n"                       // Radio Button 0
        "<Find shortest XREF Signature for current data or code address:R>\n"	// Radio Button 1
        "<Copy selected code:R>>\n"                                             // Radio Button 2

        "Output format:\n"                                                      // Title
        "<IDA Signature:R>\n"				                                    // Radio Button 0
        "<x64Dbg Signature:R>\n"			                                    // Radio Button 1
        "<C Byte Array Signature + String mask:R>\n"			                // Radio Button 2
        "<C Raw Bytes Signature + Bitmask:R>>\n"			                    // Radio Button 3

        "Options:\n"                                                            // Title
        "<Wildcards for operands:C>>\n\n";                                      // Checkbox Button

    static short action = 0;
    static short outputFormat = 0;
    static short wildcardOperands = 1;
    if( ask_form( format, &action, &outputFormat, &wildcardOperands ) ) {
        const auto sigType = static_cast< SignatureType >( outputFormat );
        switch( action ) {
        case 0:
        {
            // Find unique signature for current address
            const auto ea = get_screen_ea();
            auto signature = GenerateSignatureForEA( ea, wildcardOperands );
            PrintSignatureForEA( signature, ea, sigType );
            break;
        }
        case 1:
        {
            // Iterate XREFs and find shortest signature
            const auto ea = get_screen_ea();
            std::vector<std::tuple<ea_t, Signature>> xrefSignatures;
            FindXRefs( ea, wildcardOperands, xrefSignatures, 250 );
            PrintXRefSignaturesForEA( ea, xrefSignatures, sigType, 5 );
            break;
        }
        case 2:
        {
            // Print selected code as signature
            ea_t start, end;
            if( read_range_selection( get_current_viewer(), &start, &end ) ) {
                PrintSelectedCode( start, end, sigType );
            }
            break;
        }
        default:
            break;
        }
    }
    return true;
}

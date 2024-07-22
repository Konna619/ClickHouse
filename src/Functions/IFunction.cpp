#include <Functions/IFunctionAdaptors.h>

#include <Common/typeid_cast.h>
#include <Common/assert_cast.h>
#include <Common/SipHash.h>
#include <Core/Block.h>
#include <Core/TypeId.h>
#include <Columns/ColumnConst.h>
#include <Columns/ColumnNullable.h>
#include <Columns/ColumnTuple.h>
#include <Columns/ColumnLowCardinality.h>
#include <Columns/ColumnSparse.h>
#include <Columns/ColumnNothing.h>
#include <DataTypes/DataTypeNothing.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/Native.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <Functions/FunctionHelpers.h>
#include <cstdlib>
#include <memory>

#include "config.h"

#if USE_EMBEDDED_COMPILER
#    include <llvm/IR/IRBuilder.h>
#endif


namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int ILLEGAL_COLUMN;
}

namespace
{

bool allArgumentsAreConstants(const ColumnsWithTypeAndName & args)
{
    for (const auto & arg : args)
        if (!isColumnConst(*arg.column))
            return false;
    return true;
}

/// Replaces single low cardinality column in a function call by its dictionary
/// This can only happen after the arguments have been adapted in IFunctionOverloadResolver::getReturnType
/// as it's only possible if there is one low cardinality column and, optionally, const columns
ColumnPtr replaceLowCardinalityColumnsByNestedAndGetDictionaryIndexes(
    ColumnsWithTypeAndName & args, bool can_be_executed_on_default_arguments, size_t input_rows_count)
{
    /// We return the LC indexes so the LC can be reconstructed with the function result
    ColumnPtr indexes;

    size_t number_low_cardinality_columns = 0;
    size_t last_low_cardinality = 0;
    size_t number_const_columns = 0;
    size_t number_full_columns = 0;

    for (size_t i = 0; i < args.size(); i++)
    {
        auto const & arg = args[i];
        if (checkAndGetColumn<ColumnLowCardinality>(arg.column.get()))
        {
            number_low_cardinality_columns++;
            last_low_cardinality = i;
        }
        else if (checkAndGetColumn<ColumnConst>(arg.column.get()))
            number_const_columns++;
        else
            number_full_columns++;
    }

    if (!number_low_cardinality_columns && !number_const_columns)
        return nullptr;

    if (number_full_columns > 0 || number_low_cardinality_columns > 1)
    {
        /// This should not be possible but currently there are multiple tests in CI failing because of it
        /// TODO: Fix those cases, then enable this exception
#if 0
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected low cardinality types found. Low cardinality: {}. Full {}. Const {}",
                number_low_cardinality_columns, number_full_columns, number_const_columns);
#else
        return nullptr;
#endif
    }
    else if (number_low_cardinality_columns == 1)
    {
        auto & lc_arg = args[last_low_cardinality];

        const auto * low_cardinality_type = checkAndGetDataType<DataTypeLowCardinality>(lc_arg.type.get());
        if (!low_cardinality_type)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Incompatible type for LowCardinality column: {}", lc_arg.type->getName());

        const auto * low_cardinality_column = checkAndGetColumn<ColumnLowCardinality>(lc_arg.column.get());
        chassert(low_cardinality_column);

        if (can_be_executed_on_default_arguments)
        {
            /// Normal case, when function can be executed on values' default.
            lc_arg.column = low_cardinality_column->getDictionary().getNestedColumn();
            indexes = low_cardinality_column->getIndexesPtr();
        }
        else
        {
            /// Special case when default value can't be used. Example: 1 % LowCardinality(Int).
            /// LowCardinality always contains default, so 1 % 0 will throw exception in normal case.
            auto dict_encoded = low_cardinality_column->getMinimalDictionaryEncodedColumn(0, low_cardinality_column->size());
            lc_arg.column = dict_encoded.dictionary;
            indexes = dict_encoded.indexes;
        }

        /// The new column will have a different number of rows, normally less but occasionally it might be more (NULL)
        input_rows_count = lc_arg.column->size();
        lc_arg.type = low_cardinality_type->getDictionaryType();
    }

    /// Change size of constants
    for (auto & column : args)
    {
        if (const auto * column_const = checkAndGetColumn<ColumnConst>(column.column.get()))
        {
            column.column = ColumnConst::create(recursiveRemoveLowCardinality(column_const->getDataColumnPtr()), input_rows_count);
            column.type = recursiveRemoveLowCardinality(column.type);
        }
    }

    return indexes;
}

void convertLowCardinalityColumnsToFull(ColumnsWithTypeAndName & args)
{
    for (auto & column : args)
    {
        column.column = recursiveRemoveLowCardinality(column.column);
        column.type = recursiveRemoveLowCardinality(column.type);
    }
}

}

ColumnPtr IExecutableFunction::defaultImplementationForConstantArguments(
    const ColumnsWithTypeAndName & args, const DataTypePtr & result_type, size_t input_rows_count, bool dry_run) const
{
    ColumnNumbers arguments_to_remain_constants = getArgumentsThatAreAlwaysConstant();

    /// Check that these arguments are really constant.
    for (auto arg_num : arguments_to_remain_constants)
        if (arg_num < args.size() && !isColumnConst(*args[arg_num].column))
            throw Exception(ErrorCodes::ILLEGAL_COLUMN,
                "Argument at index {} for function {} must be constant",
                arg_num,
                getName());

    if (args.empty() || !useDefaultImplementationForConstants() || !allArgumentsAreConstants(args))
        return nullptr;

    ColumnsWithTypeAndName temporary_columns;
    bool have_converted_columns = false;

    size_t arguments_size = args.size();
    temporary_columns.reserve(arguments_size);
    for (size_t arg_num = 0; arg_num < arguments_size; ++arg_num)
    {
        const ColumnWithTypeAndName & column = args[arg_num];

        if (arguments_to_remain_constants.end() != std::find(arguments_to_remain_constants.begin(), arguments_to_remain_constants.end(), arg_num))
        {
            temporary_columns.emplace_back(ColumnWithTypeAndName{column.column->cloneResized(1), column.type, column.name});
        }
        else
        {
            have_converted_columns = true;
            temporary_columns.emplace_back(ColumnWithTypeAndName{ assert_cast<const ColumnConst *>(column.column.get())->getDataColumnPtr(), column.type, column.name });
        }
    }

    /** When using default implementation for constants, the function requires at least one argument
      *  not in "arguments_to_remain_constants" set. Otherwise we get infinite recursion.
      */
    if (!have_converted_columns)
        throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
            "Number of arguments for function {} doesn't match: the function requires more arguments",
            getName());

    ColumnPtr result_column = executeWithoutLowCardinalityColumns(temporary_columns, result_type, 1, dry_run);

    /// extremely rare case, when we have function with completely const arguments
    /// but some of them produced by non isDeterministic function
    if (result_column->size() > 1)
        result_column = result_column->cloneResized(1);

    return ColumnConst::create(result_column, input_rows_count);
}


ColumnPtr IExecutableFunction::defaultImplementationForNulls(
    const ColumnsWithTypeAndName & args, const DataTypePtr & result_type, size_t input_rows_count, bool dry_run) const
{
    if (args.empty() || !useDefaultImplementationForNulls())
        return nullptr;

    NullPresence null_presence = getNullPresense(args);

    if (null_presence.has_null_constant)
    {
        // Default implementation for nulls returns null result for null arguments,
        // so the result type must be nullable.
        if (!result_type->isNullable())
            throw Exception(
                ErrorCodes::LOGICAL_ERROR,
                "Function {} with Null argument and default implementation for Nulls "
                "is expected to return Nullable result, got {}",
                getName(),
                result_type->getName());

        return result_type->createColumnConstWithDefaultValue(input_rows_count);
    }

    if (null_presence.has_nullable)
    {
        ColumnsWithTypeAndName temporary_columns = createBlockWithNestedColumns(args);
        auto temporary_result_type = removeNullable(result_type);

        auto res = executeWithoutLowCardinalityColumns(temporary_columns, temporary_result_type, input_rows_count, dry_run);
        return wrapInNullable(res, args, result_type, input_rows_count);
    }

    return nullptr;
}

ColumnPtr IExecutableFunction::defaultImplementationForNothing(
    const ColumnsWithTypeAndName & args, const DataTypePtr & result_type, size_t input_rows_count) const
{
    if (!useDefaultImplementationForNothing())
        return nullptr;

    bool is_nothing_type_presented = false;
    for (const auto & arg : args)
        is_nothing_type_presented |= isNothing(arg.type);

    if (!is_nothing_type_presented)
        return nullptr;

    if (!isNothing(result_type))
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "Function {} with argument with type Nothing and default implementation for Nothing "
            "is expected to return result with type Nothing, got {}",
            getName(),
            result_type->getName());

    if (input_rows_count > 0)
        throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Cannot create non-empty column with type Nothing");
    return ColumnNothing::create(0);
}

ColumnPtr IExecutableFunction::executeWithoutLowCardinalityColumns(
    const ColumnsWithTypeAndName & args, const DataTypePtr & result_type, size_t input_rows_count, bool dry_run) const
{
    if (auto res = defaultImplementationForNothing(args, result_type, input_rows_count))
        return res;

    if (auto res = defaultImplementationForConstantArguments(args, result_type, input_rows_count, dry_run))
        return res;

    if (auto res = defaultImplementationForNulls(args, result_type, input_rows_count, dry_run))
        return res;

    ColumnPtr res;
    if (dry_run)
        res = executeDryRunImpl(args, result_type, input_rows_count);
    else
        res = executeImpl(args, result_type, input_rows_count);

    if (!res)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Empty column was returned by function {}", getName());

    return res;
}

static void convertSparseColumnsToFull(ColumnsWithTypeAndName & args)
{
    for (auto & column : args)
        column.column = recursiveRemoveSparse(column.column);
}

ColumnPtr IExecutableFunction::executeWithoutSparseColumns(const ColumnsWithTypeAndName & arguments, const DataTypePtr & result_type, size_t input_rows_count, bool dry_run) const
{
    ColumnPtr result;
    if (useDefaultImplementationForLowCardinalityColumns())
    {
        ColumnsWithTypeAndName columns_without_low_cardinality = arguments;

        if (const auto * res_low_cardinality_type = typeid_cast<const DataTypeLowCardinality *>(result_type.get()))
        {
            bool can_be_executed_on_default_arguments = canBeExecutedOnDefaultArguments();

            const auto & dictionary_type = res_low_cardinality_type->getDictionaryType();
            /// The arguments should have been adapted in IFunctionOverloadResolver::getReturnType
            /// So there is only one low cardinality column (and optionally some const columns) and no full column
            ColumnPtr indexes = replaceLowCardinalityColumnsByNestedAndGetDictionaryIndexes(
                    columns_without_low_cardinality, can_be_executed_on_default_arguments, input_rows_count);

            size_t new_input_rows_count = columns_without_low_cardinality.empty()
                                        ? input_rows_count
                                        : columns_without_low_cardinality.front().column->size();

            auto res = executeWithoutLowCardinalityColumns(columns_without_low_cardinality, dictionary_type, new_input_rows_count, dry_run);
            bool res_is_constant = isColumnConst(*res);

            auto keys = res_is_constant
                ? res->cloneResized(1)->convertToFullColumnIfConst()
                : res;

            auto res_mut_dictionary = DataTypeLowCardinality::createColumnUnique(*res_low_cardinality_type->getDictionaryType());
            ColumnPtr res_indexes = res_mut_dictionary->uniqueInsertRangeFrom(*keys, 0, keys->size());
            ColumnUniquePtr res_dictionary = std::move(res_mut_dictionary);

            if (indexes && !res_is_constant)
                result = ColumnLowCardinality::create(res_dictionary, res_indexes->index(*indexes, 0));
            else
                result = ColumnLowCardinality::create(res_dictionary, res_indexes);

            if (res_is_constant)
                result = ColumnConst::create(std::move(result), input_rows_count);
        }
        else
        {
            convertLowCardinalityColumnsToFull(columns_without_low_cardinality);
            result = executeWithoutLowCardinalityColumns(columns_without_low_cardinality, result_type, input_rows_count, dry_run);
        }
    }
    else
        result = executeWithoutLowCardinalityColumns(arguments, result_type, input_rows_count, dry_run);

    return result;
}

ColumnPtr IExecutableFunction::execute(const ColumnsWithTypeAndName & arguments, const DataTypePtr & result_type, size_t input_rows_count, bool dry_run) const
{
    bool use_default_implementation_for_sparse_columns = useDefaultImplementationForSparseColumns();
    /// DataTypeFunction does not support obtaining default (isDefaultAt())
    /// ColumnFunction does not support getting specific values.
    if (result_type->getTypeId() != TypeIndex::Function && use_default_implementation_for_sparse_columns)
    {
        size_t num_sparse_columns = 0;
        size_t num_full_columns = 0;
        size_t sparse_column_position = 0;

        for (size_t i = 0; i < arguments.size(); ++i)
        {
            const auto * column_sparse = checkAndGetColumn<ColumnSparse>(arguments[i].column.get());
            /// In rare case, when sparse column doesn't have default values,
            /// it's more convenient to convert it to full before execution of function.
            if (column_sparse && column_sparse->getNumberOfDefaultRows())
            {
                sparse_column_position = i;
                ++num_sparse_columns;
            }
            else if (!isColumnConst(*arguments[i].column))
            {
                ++num_full_columns;
            }
        }

        auto columns_without_sparse = arguments;
        if (num_sparse_columns == 1 && num_full_columns == 0)
        {
            auto & arg_with_sparse = columns_without_sparse[sparse_column_position];
            ColumnPtr sparse_offsets;
            {
                /// New scope to avoid possible mistakes on dangling reference.
                const auto & column_sparse = assert_cast<const ColumnSparse &>(*arg_with_sparse.column);
                sparse_offsets = column_sparse.getOffsetsPtr();
                arg_with_sparse.column = column_sparse.getValuesPtr();
            }

            size_t values_size = arg_with_sparse.column->size();
            for (size_t i = 0; i < columns_without_sparse.size(); ++i)
            {
                if (i == sparse_column_position)
                    continue;

                columns_without_sparse[i].column = columns_without_sparse[i].column->cloneResized(values_size);
            }

            auto res = executeWithoutSparseColumns(columns_without_sparse, result_type, values_size, dry_run);

            if (isColumnConst(*res))
                return res->cloneResized(input_rows_count);

            /// If default of sparse column is changed after execution of function, convert to full column.
            /// If there are any default in non-zero position after execution of function, convert to full column.
            /// Currently there is no easy way to rebuild sparse column with new offsets.
            if (!result_type->canBeInsideSparseColumns() || !res->isDefaultAt(0) || res->getNumberOfDefaultRows() != 1)
            {
                const auto & offsets_data = assert_cast<const ColumnVector<UInt64> &>(*sparse_offsets).getData();
                return res->createWithOffsets(offsets_data, *createColumnConst(res, 0), input_rows_count, /*shift=*/ 1);
            }

            return ColumnSparse::create(res, sparse_offsets, input_rows_count);
        }

        convertSparseColumnsToFull(columns_without_sparse);
        return executeWithoutSparseColumns(columns_without_sparse, result_type, input_rows_count, dry_run);
    }
    else if (use_default_implementation_for_sparse_columns)
    {
        auto columns_without_sparse = arguments;
        convertSparseColumnsToFull(columns_without_sparse);
        return executeWithoutSparseColumns(columns_without_sparse, result_type, input_rows_count, dry_run);
    }
    else
        return executeWithoutSparseColumns(arguments, result_type, input_rows_count, dry_run);
}

void IFunctionOverloadResolver::checkNumberOfArguments(size_t number_of_arguments) const
{
    if (isVariadic())
        return;

    size_t expected_number_of_arguments = getNumberOfArguments();

    if (number_of_arguments != expected_number_of_arguments)
        throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
            "Number of arguments for function {} doesn't match: passed {}, should be {}",
            getName(),
            number_of_arguments,
            expected_number_of_arguments);
}

DataTypePtr IFunctionOverloadResolver::getReturnType(const ColumnsWithTypeAndName & arguments) const
{
    if (useDefaultImplementationForLowCardinalityColumns())
    {
        bool has_low_cardinality = false;
        size_t num_full_low_cardinality_columns = 0;
        size_t num_full_ordinary_columns = 0;

        ColumnsWithTypeAndName args_without_low_cardinality(arguments);

        for (ColumnWithTypeAndName & arg : args_without_low_cardinality)
        {
            bool is_const = arg.column && isColumnConst(*arg.column);
            if (is_const)
                arg.column = assert_cast<const ColumnConst &>(*arg.column).removeLowCardinality();

            if (const auto * low_cardinality_type = typeid_cast<const DataTypeLowCardinality *>(arg.type.get()))
            {
                arg.type = low_cardinality_type->getDictionaryType();
                has_low_cardinality = true;

                if (!is_const)
                    ++num_full_low_cardinality_columns;
            }
            else if (!is_const)
                ++num_full_ordinary_columns;
        }

        convertLowCardinalityColumnsToFull(args_without_low_cardinality);

        auto type_without_low_cardinality = getReturnTypeWithoutLowCardinality(args_without_low_cardinality);

        if (canBeExecutedOnLowCardinalityDictionary() && has_low_cardinality
            && num_full_low_cardinality_columns <= 1 && num_full_ordinary_columns == 0
            && type_without_low_cardinality->canBeInsideLowCardinality())
            return std::make_shared<DataTypeLowCardinality>(type_without_low_cardinality);
        else
            return type_without_low_cardinality;
    }

    return getReturnTypeWithoutLowCardinality(arguments);
}

FunctionBasePtr IFunctionOverloadResolver::build(const ColumnsWithTypeAndName & arguments) const
{
    auto return_type = getReturnType(arguments);
    return buildImpl(arguments, return_type);
}

void IFunctionOverloadResolver::getLambdaArgumentTypes(DataTypes & arguments [[maybe_unused]]) const
{
    checkNumberOfArguments(arguments.size());
    getLambdaArgumentTypesImpl(arguments);
}

DataTypePtr IFunctionOverloadResolver::getReturnTypeWithoutLowCardinality(const ColumnsWithTypeAndName & arguments) const
{
    checkNumberOfArguments(arguments.size());

    if (!arguments.empty() && useDefaultImplementationForNothing())
    {
        for (const auto & arg : arguments)
        {
            if (isNothing(arg.type))
                return std::make_shared<DataTypeNothing>();
        }
    }

    if (!arguments.empty() && useDefaultImplementationForNulls())
    {
        NullPresence null_presence = getNullPresense(arguments);

        if (null_presence.has_null_constant)
        {
            return makeNullable(std::make_shared<DataTypeNothing>());
        }
        if (null_presence.has_nullable)
        {
            Block nested_columns = createBlockWithNestedColumns(arguments);
            auto return_type = getReturnTypeImpl(ColumnsWithTypeAndName(nested_columns.begin(), nested_columns.end()));
            return makeNullable(return_type);
        }
    }

    return getReturnTypeImpl(arguments);
}


#if USE_EMBEDDED_COMPILER

static std::optional<DataTypes> removeNullables(const DataTypes & types)
{
    bool has_nullable = false;
    for (const auto & type : types)
    {
        if (!typeid_cast<const DataTypeNullable *>(type.get()))
            continue;

        has_nullable = true;
        break;
    }

    if (has_nullable)
    {
        DataTypes filtered;
        filtered.reserve(types.size());

        for (const auto & sub_type : types)
            filtered.emplace_back(removeNullable(sub_type));

        return filtered;
    }

    return {};
}

bool IFunction::isCompilable(const DataTypes & arguments, const DataTypePtr & result_type) const
{
    if (useDefaultImplementationForNulls())
        if (auto denulled_arguments = removeNullables(arguments))
            return isCompilableImpl(*denulled_arguments, result_type);

    return isCompilableImpl(arguments, result_type);
}

llvm::Value * IFunction::compile(llvm::IRBuilderBase & builder, const ValuesWithType & arguments, const DataTypePtr & result_type) const
{
    DataTypes arguments_types;
    arguments_types.reserve(arguments.size());

    for (const auto & argument : arguments)
        arguments_types.push_back(argument.type);

    auto denulled_arguments_types = removeNullables(arguments_types);
    if (useDefaultImplementationForNulls() && denulled_arguments_types)
    {
        auto & b = static_cast<llvm::IRBuilder<> &>(builder);

        ValuesWithType unwrapped_arguments;
        unwrapped_arguments.reserve(arguments.size());

        std::vector<llvm::Value*> is_null_values;

        for (size_t i = 0; i < arguments.size(); ++i)
        {
            const auto & argument = arguments[i];
            llvm::Value * unwrapped_value = argument.value;

            if (argument.type->isNullable())
            {
                unwrapped_value = b.CreateExtractValue(argument.value, {0});
                is_null_values.emplace_back(b.CreateExtractValue(argument.value, {1}));
            }

            unwrapped_arguments.emplace_back(unwrapped_value, (*denulled_arguments_types)[i]);
        }

        auto * result = compileImpl(builder, unwrapped_arguments, removeNullable(result_type));

        auto * nullable_structure_type = toNativeType(b, makeNullable(getReturnTypeImpl(*denulled_arguments_types)));
        auto * nullable_structure_value = llvm::Constant::getNullValue(nullable_structure_type);

        auto * nullable_structure_with_result_value = b.CreateInsertValue(nullable_structure_value, result, {0});
        auto * nullable_structure_result_null = b.CreateExtractValue(nullable_structure_with_result_value, {1});

        for (auto * is_null_value : is_null_values)
            nullable_structure_result_null = b.CreateOr(nullable_structure_result_null, is_null_value);

        return b.CreateInsertValue(nullable_structure_with_result_value, nullable_structure_result_null, {1});
    }

    return compileImpl(builder, arguments, result_type);
}

#endif

}

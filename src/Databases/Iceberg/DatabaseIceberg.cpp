#include <Databases/Iceberg/DatabaseIceberg.h>

#if USE_AVRO
#include <Access/Common/HTTPAuthenticationScheme.h>

#include <Databases/DatabaseFactory.h>
#include <Databases/Iceberg/RestCatalog.h>
#include <DataTypes/DataTypeString.h>

#include <Storages/ObjectStorage/S3/Configuration.h>
#include <Storages/ConstraintsDescription.h>
#include <Storages/StorageNull.h>
#include <Storages/ObjectStorage/DataLakes/DataLakeConfiguration.h>

#include <Interpreters/evaluateConstantExpression.h>
#include <Interpreters/Context.h>
#include <Interpreters/StorageID.h>

#include <Formats/FormatFactory.h>

#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTDataType.h>

namespace CurrentMetrics
{
    extern const Metric IcebergCatalogThreads;
    extern const Metric IcebergCatalogThreadsActive;
    extern const Metric IcebergCatalogThreadsScheduled;
}


namespace DB
{
namespace DatabaseIcebergSetting
{
    extern const DatabaseIcebergSettingsDatabaseIcebergCatalogType catalog_type;
    extern const DatabaseIcebergSettingsDatabaseIcebergStorageType storage_type;
    extern const DatabaseIcebergSettingsString warehouse;
    extern const DatabaseIcebergSettingsString catalog_credential;
    extern const DatabaseIcebergSettingsString auth_header;
    extern const DatabaseIcebergSettingsString auth_scope;
    extern const DatabaseIcebergSettingsString storage_endpoint;
}

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}

namespace
{
    /// Parse a string, containing at least one dot, into a two substrings:
    /// A.B.C.D.E -> A.B.C.D and E, where
    /// `A.B.C.D` is a table "namespace".
    /// `E` is a table name.
    std::pair<std::string, std::string> parseTableName(const std::string & name)
    {
        auto pos = name.rfind('.');
        if (pos == std::string::npos)
            throw DB::Exception(ErrorCodes::BAD_ARGUMENTS, "Table cannot have empty namespace: {}", name);

        auto table_name = name.substr(pos + 1);
        auto namespace_name = name.substr(0, name.size() - table_name.size() - 1);
        return {namespace_name, table_name};
    }
}

DatabaseIceberg::DatabaseIceberg(
    const std::string & database_name_,
    const std::string & url_,
    const DatabaseIcebergSettings & settings_,
    ASTPtr database_engine_definition_,
    ContextPtr context_)
    : IDatabase(database_name_)
    , url(url_)
    , settings(settings_)
    , database_engine_definition(database_engine_definition_)
    , log(getLogger("DatabaseIceberg(" + database_name_ + ")"))
{
    validateSettings(context_);
}

void DatabaseIceberg::validateSettings(const ContextPtr & context_)
{
    if (settings[DatabaseIcebergSetting::warehouse].value.empty())
    {
        throw Exception(
            ErrorCodes::BAD_ARGUMENTS, "`warehouse` setting cannot be empty. "
            "Please specify 'SETTINGS warehouse=<warehouse_name>' in the CREATE DATABASE query");
    }

    if (!settings[DatabaseIcebergSetting::storage_type].changed)
    {
        auto catalog = getCatalog(context_);
        const auto storage_type = catalog->getStorageType();
        if (!storage_type)
        {
            throw Exception(
                ErrorCodes::BAD_ARGUMENTS, "Storage type is not found in catalog config. "
                "Please specify it manually via 'SETTINGS storage_type=<type>' in CREATE DATABASE query");
        }
    }
}

std::shared_ptr<Iceberg::ICatalog> DatabaseIceberg::getCatalog(ContextPtr) const
{
    if (catalog_impl)
        return catalog_impl;

    switch (settings[DatabaseIcebergSetting::catalog_type].value)
    {
        case DB::DatabaseIcebergCatalogType::REST:
        {
            catalog_impl = std::make_shared<Iceberg::RestCatalog>(
                settings[DatabaseIcebergSetting::warehouse].value,
                url,
                settings[DatabaseIcebergSetting::catalog_credential].value,
                settings[DatabaseIcebergSetting::auth_scope].value,
                settings[DatabaseIcebergSetting::auth_header],
                Context::getGlobalContextInstance());
        }
    }
    return catalog_impl;
}

std::shared_ptr<StorageObjectStorage::Configuration> DatabaseIceberg::getConfiguration() const
{
    /// TODO: add tests for azure, local storage types.

    switch (settings[DatabaseIcebergSetting::storage_type].value)
    {
#if USE_AWS_S3
        case DB::DatabaseIcebergStorageType::S3:
        {
            return std::make_shared<StorageS3IcebergConfiguration>();
        }
#endif
#if USE_AZURE_BLOB_STORAGE
        case DB::DatabaseIcebergStorageType::Azure:
        {
            return std::make_shared<StorageAzureIcebergConfiguration>();
        }
#endif
#if USE_HDFS
        case DB::DatabaseIcebergStorageType::HDFS:
        {
            return std::make_shared<StorageHDFSIcebergConfiguration>();
        }
#endif
        case DB::DatabaseIcebergStorageType::Local:
        {
            return std::make_shared<StorageLocalIcebergConfiguration>();
        }
#if !USE_AWS_S3 || !USE_AZURE_BLOB_STORAGE || !USE_HDFS
        default:
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                            "Server does not contain support for storage type {}",
                            settings[DatabaseIcebergSetting::storage_type].value);
#endif
    }
}

std::string DatabaseIceberg::getStorageEndpointForTable(const Iceberg::TableMetadata & table_metadata) const
{
    auto endpoint_from_settings = settings[DatabaseIcebergSetting::storage_endpoint].value;
    if (!endpoint_from_settings.empty())
    {
        return std::filesystem::path(endpoint_from_settings)
            / table_metadata.getLocation(/* path_only */true)
            / "";
    }
    else
    {
        return std::filesystem::path(table_metadata.getLocation(/* path_only */false)) / "";
    }
}

bool DatabaseIceberg::empty() const
{
    return getCatalog(Context::getGlobalContextInstance())->empty();
}

bool DatabaseIceberg::isTableExist(const String & name, ContextPtr context_) const
{
    const auto [namespace_name, table_name] = parseTableName(name);
    return getCatalog(context_)->existsTable(namespace_name, table_name);
}

StoragePtr DatabaseIceberg::tryGetTable(const String & name, ContextPtr context_) const
{
    auto catalog = getCatalog(context_);
    auto table_metadata = Iceberg::TableMetadata().withLocation().withSchema();
    auto [namespace_name, table_name] = parseTableName(name);

    if (!catalog->tryGetTableMetadata(namespace_name, table_name, table_metadata))
        return nullptr;

    /// Take database engine definition AST as base.
    ASTStorage * storage = database_engine_definition->as<ASTStorage>();
    ASTs args = storage->engine->arguments->children;

    /// Replace Iceberg Catalog endpoint with storage path endpoint of requested table.
    auto table_endpoint = getStorageEndpointForTable(table_metadata);
    args[0] = std::make_shared<ASTLiteral>(table_endpoint);

    LOG_TEST(log, "Using table endpoint: {}", table_endpoint);

    const auto columns = ColumnsDescription(table_metadata.getSchema());
    const auto configuration = getConfiguration();

    /// with_table_structure = false: because there will be
    /// no table structure in table definition AST.
    StorageObjectStorage::Configuration::initialize(*configuration, args, context_, /* with_table_structure */false);

    return std::make_shared<StorageObjectStorage>(
        configuration,
        configuration->createObjectStorage(context_, /* is_readonly */ false),
        context_,
        StorageID(getDatabaseName(), name),
        /* columns */columns,
        /* constraints */ConstraintsDescription{},
        /* comment */"",
        getFormatSettings(context_),
        LoadingStrictnessLevel::CREATE,
        /* distributed_processing */false,
        /* partition_by */nullptr,
        /* lazy_init */true);
}

DatabaseTablesIteratorPtr DatabaseIceberg::getTablesIterator(
    ContextPtr context_,
    const FilterByNameFunction & filter_by_table_name,
    bool /* skip_not_loaded */) const
{
    Tables tables;
    auto catalog = getCatalog(context_);
    const auto iceberg_tables = catalog->getTables();

    auto & pool = getContext()->getIcebergCatalogThreadpool();
    DB::ThreadPoolCallbackRunnerLocal<void> runner(pool, "RestCatalog");
    std::mutex mutexx;

    for (const auto & table_name : iceberg_tables)
    {
        if (filter_by_table_name && !filter_by_table_name(table_name))
            continue;

        runner([=, &tables, &mutexx, this]{
            auto storage = tryGetTable(table_name, context_);
            {
                std::lock_guard lock(mutexx);
                [[maybe_unused]] bool inserted = tables.emplace(table_name, storage).second;
                chassert(inserted);
            }
        });
    }

    runner.waitForAllToFinishAndRethrowFirstError();
    return std::make_unique<DatabaseTablesSnapshotIterator>(tables, getDatabaseName());
}

ASTPtr DatabaseIceberg::getCreateDatabaseQuery() const
{
    const auto & create_query = std::make_shared<ASTCreateQuery>();
    create_query->setDatabase(getDatabaseName());
    create_query->set(create_query->storage, database_engine_definition);
    return create_query;
}

ASTPtr DatabaseIceberg::getCreateTableQueryImpl(
    const String & name,
    ContextPtr context_,
    bool /* throw_on_error */) const
{
    auto catalog = getCatalog(context_);
    auto table_metadata = Iceberg::TableMetadata().withLocation().withSchema();

    const auto [namespace_name, table_name] = parseTableName(name);
    catalog->getTableMetadata(namespace_name, table_name, table_metadata);

    auto create_table_query = std::make_shared<ASTCreateQuery>();
    auto table_storage_define = database_engine_definition->clone();

    auto * storage = table_storage_define->as<ASTStorage>();
    storage->engine->kind = ASTFunction::Kind::TABLE_ENGINE;
    storage->settings = {};

    create_table_query->set(create_table_query->storage, table_storage_define);

    auto columns_declare_list = std::make_shared<ASTColumns>();
    auto columns_expression_list = std::make_shared<ASTExpressionList>();

    columns_declare_list->set(columns_declare_list->columns, columns_expression_list);
    create_table_query->set(create_table_query->columns_list, columns_declare_list);

    create_table_query->setTable(name);
    create_table_query->setDatabase(getDatabaseName());

    for (const auto & column_type_and_name : table_metadata.getSchema())
    {
        const auto column_declaration = std::make_shared<ASTColumnDeclaration>();
        column_declaration->name = column_type_and_name.name;
        column_declaration->type = makeASTDataType(column_type_and_name.type->getName());
        columns_expression_list->children.emplace_back(column_declaration);
    }

    auto storage_engine_arguments = storage->engine->arguments;
    if (storage_engine_arguments->children.empty())
    {
        throw Exception(
            ErrorCodes::BAD_ARGUMENTS, "Unexpected number of arguments: {}",
            storage_engine_arguments->children.size());
    }

    auto table_endpoint = getStorageEndpointForTable(table_metadata);
    storage_engine_arguments->children[0] = std::make_shared<ASTLiteral>(table_endpoint);

    return create_table_query;
}

void registerDatabaseIceberg(DatabaseFactory & factory)
{
    auto create_fn = [](const DatabaseFactory::Arguments & args)
    {
        const auto * database_engine_define = args.create_query.storage;
        const auto & database_engine_name = args.engine_name;

        const ASTFunction * function_define = database_engine_define->engine;
        if (!function_define->arguments)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Engine `{}` must have arguments", database_engine_name);

        ASTs & engine_args = function_define->arguments->children;
        if (engine_args.empty())
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Engine `{}` must have arguments", database_engine_name);

        const size_t max_args_num = 3;
        if (engine_args.size() != max_args_num)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Engine must have {} arguments", max_args_num);

        for (auto & engine_arg : engine_args)
            engine_arg = evaluateConstantExpressionOrIdentifierAsLiteral(engine_arg, args.context);

        const auto url = engine_args[0]->as<ASTLiteral>()->value.safeGet<String>();

        DatabaseIcebergSettings database_settings;
        if (database_engine_define->settings)
            database_settings.loadFromQuery(*database_engine_define);

        return std::make_shared<DatabaseIceberg>(
            args.database_name,
            url,
            database_settings,
            database_engine_define->clone(),
            args.context);
    };
    factory.registerDatabase("Iceberg", create_fn, { .supports_arguments = true, .supports_settings = true });
}

}

#endif

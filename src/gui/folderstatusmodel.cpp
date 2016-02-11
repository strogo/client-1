/*
 * Copyright (C) by Klaas Freitag <freitag@kde.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "folderstatusmodel.h"
#include "folderman.h"
#include "accountstate.h"
#include "utility.h"
#include <theme.h>
#include <account.h>
#include "folderstatusdelegate.h"

#include <QFileIconProvider>
#include <QVarLengthArray>
#include <set>

Q_DECLARE_METATYPE(QPersistentModelIndex)

namespace OCC {

static const char propertyParentIndexC[] = "oc_parentIndex";

FolderStatusModel::FolderStatusModel(QObject *parent)
    :QAbstractItemModel(parent), _accountState(0), _dirty(false)
{
}

FolderStatusModel::~FolderStatusModel()
{ }


void FolderStatusModel::setAccountState(const AccountState* accountState)
{
    beginResetModel();
    _dirty = false;
    _folders.clear();
    _accountState = accountState;

    connect(FolderMan::instance(), SIGNAL(folderSyncStateChange(Folder*)),
            SLOT(slotFolderSyncStateChange(Folder*)), Qt::UniqueConnection);
    connect(FolderMan::instance(), SIGNAL(scheduleQueueChanged()),
            SLOT(slotFolderScheduleQueueChanged()), Qt::UniqueConnection);

    auto folders = FolderMan::instance()->map();
    foreach (auto f, folders) {
		if (!accountState)
			break;
        if (f->accountState() != accountState)
            continue;
        SubFolderInfo info;
        info._pathIdx << _folders.size();
        info._name = f->alias();
        info._path = "/";
        info._folder = f;
        info._checked = Qt::PartiallyChecked;
        _folders << info;

        connect(f, SIGNAL(progressInfo(ProgressInfo)), this, SLOT(slotSetProgress(ProgressInfo)), Qt::UniqueConnection);
        connect(f, SIGNAL(newBigFolderDiscovered(QString)), this, SLOT(slotNewBigFolder()), Qt::UniqueConnection);
    }

    endResetModel();
    emit dirtyChanged();
}


Qt::ItemFlags FolderStatusModel::flags ( const QModelIndex &index  ) const
{
	if (!_accountState) {
		return 0;
	}
    switch (classify(index)) {
        case AddButton: {
            Qt::ItemFlags ret;
#if QT_VERSION >= QT_VERSION_CHECK(5, 1, 0)
            ret = Qt::ItemNeverHasChildren;
#endif
            if (!_accountState->isConnected()) {
                return ret;
            } else if (_folders.count() == 1) {
                auto remotePath = _folders.at(0)._folder->remotePath();
                // special case when syncing the entire owncloud: disable the add folder button (#3438)
                if (remotePath.isEmpty() || remotePath == QLatin1String("/")) {
                    return ret;
                }
            }
            return Qt::ItemIsEnabled | ret;
        }
        case FetchLabel:
            return Qt::ItemIsEnabled
#if QT_VERSION >= QT_VERSION_CHECK(5, 1, 0)
                    | Qt::ItemNeverHasChildren
#endif
                    ;
        case RootFolder:
            return  Qt::ItemIsEnabled;
        case SubFolder:
            return  Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable;
    }
    return 0;
}

QVariant FolderStatusModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return QVariant();

    if (role == Qt::EditRole)
        return QVariant();

    switch(classify(index)) {
    case AddButton: {
        if (role == FolderStatusDelegate::AddButton) {
            return QVariant(true);
        } else if (role == Qt::ToolTipRole) {
            if (!_accountState->isConnected()) {
                return tr("You need to be connected to add a folder");
            } if (_folders.count() == 1) {
                auto remotePath = _folders.at(0)._folder->remotePath();
                if (remotePath.isEmpty() || remotePath == QLatin1String("/")) {
                    // Syncing the entire owncloud: disable the add folder button (#3438)
                    return tr("Adding folder is disabled because you are already syncing all your files. "
                            "If you want to sync multiple folders, please remove the currently "
                            "configured root folder.");
                }
            }
            return tr("Click this button to add a folder to synchronize.");
        }
        return QVariant();
    }
    case SubFolder:
    {
        const auto &x = static_cast<SubFolderInfo *>(index.internalPointer())->_subs[index.row()];
        switch (role) {
        case Qt::ToolTipRole:
        case Qt::DisplayRole:
            //: Example text: "File.txt (23KB)"
            return tr("%1 (%2)").arg(x._name, Utility::octetsToString(x._size));
        case Qt::CheckStateRole:
            return x._checked;
        case Qt::DecorationRole:
            return QFileIconProvider().icon(QFileIconProvider::Folder);
        case Qt::ForegroundRole:
            if (x._isUndecided) {
                return QColor(Qt::red);
            }
            break;
        }
    }
        return QVariant();
    case FetchLabel:
    {
        const auto x = static_cast<SubFolderInfo *>(index.internalPointer());
        switch(role) {
            case Qt::DisplayRole:
                if (x->_hasError) {
                    return tr("Error while loading the list of folders from the server.");
                } else {
                    return tr("Fetching folder list from server...");
                }
                break;
            default: return QVariant();
        }
    }
    case RootFolder:
        break;
    }

    const SubFolderInfo & folderInfo = _folders.at(index.row());
    auto f = folderInfo._folder;
    if (!f)
        return QVariant();

    const SubFolderInfo::Progress & progress = folderInfo._progress;
    const bool accountConnected = _accountState->isConnected();

    switch (role) {
    case FolderStatusDelegate::FolderPathRole         : return  f->shortGuiPath();
    case FolderStatusDelegate::FolderSecondPathRole   : return  f->remotePath();
    case FolderStatusDelegate::FolderErrorMsg         : return  f->syncResult().errorStrings();
    case FolderStatusDelegate::SyncRunning            : return  f->syncResult().status() == SyncResult::SyncRunning;
    case FolderStatusDelegate::HeaderRole             : return  f->aliasGui();
    case FolderStatusDelegate::FolderAliasRole        : return  f->alias();
    case FolderStatusDelegate::FolderSyncPaused       : return  f->syncPaused();
    case FolderStatusDelegate::FolderAccountConnected : return  accountConnected;
    case Qt::ToolTipRole: {
        QString toolTip;
        if ( accountConnected )
            toolTip = Theme::instance()->statusHeaderText(f->syncResult().status());
        else
            toolTip = tr("Signed out");
        toolTip += "\n";
        toolTip += folderInfo._folder->path();
        return toolTip;
    }
    case FolderStatusDelegate::FolderStatusIconRole:
        if ( accountConnected ) {
            auto theme = Theme::instance();
            auto status = f->syncResult().status();
            if( f->syncPaused() ) {
                return theme->folderDisabledIcon( );
            } else {
                if( status == SyncResult::SyncPrepare ) {
                    return theme->syncStateIcon(SyncResult::SyncRunning);
                } else if( status == SyncResult::Undefined ) {
                    return theme->syncStateIcon( SyncResult::SyncRunning);
                } else {
                    // keep the previous icon for the prepare phase.
                    if( status == SyncResult::Problem) {
                        return theme->syncStateIcon( SyncResult::Success);
                    } else {
                        return theme->syncStateIcon( status );
                    }
                }
            }
        } else {
            return Theme::instance()->folderOfflineIcon();
        }
    case FolderStatusDelegate::SyncProgressItemString:
        return progress._progressString;
    case FolderStatusDelegate::WarningCount:
        return progress._warningCount;
    case FolderStatusDelegate::SyncProgressOverallPercent:
        return progress._overallPercent;
    case FolderStatusDelegate::SyncProgressOverallString:
        return progress._overallSyncString;
    }
    return QVariant();
}

bool FolderStatusModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if(role == Qt::CheckStateRole) {
        auto info = infoForIndex(index);
        Qt::CheckState checked = static_cast<Qt::CheckState>(value.toInt());

        if (info && info->_checked != checked) {
            info->_checked = checked;
            if (checked == Qt::Checked) {
                // If we are checked, check that we may need to check the parent as well if
                // all the siblings are also checked
                QModelIndex parent = index.parent();
                auto parentInfo = infoForIndex(parent);
                if (parentInfo && parentInfo->_checked != Qt::Checked) {
                    bool hasUnchecked = false;
                    foreach(const auto &sub, parentInfo->_subs) {
                        if (sub._checked != Qt::Checked) {
                            hasUnchecked = true;
                            break;
                        }
                    }
                    if (!hasUnchecked) {
                        setData(parent, Qt::Checked, Qt::CheckStateRole);
                    } else if (parentInfo->_checked == Qt::Unchecked) {
                        setData(parent, Qt::PartiallyChecked, Qt::CheckStateRole);
                    }
                }
                // also check all the children
                for (int i = 0; i < info->_subs.count(); ++i) {
                    if (info->_subs[i]._checked != Qt::Checked) {
                        setData(index.child(i, 0), Qt::Checked, Qt::CheckStateRole);
                    }
                }
            }

            if (checked == Qt::Unchecked) {
                QModelIndex parent = index.parent();
                auto parentInfo = infoForIndex(parent);
                if (parentInfo && parentInfo->_checked == Qt::Checked) {
                    setData(parent, Qt::PartiallyChecked, Qt::CheckStateRole);
                }

                // Uncheck all the children
                for (int i = 0; i < info->_subs.count(); ++i) {
                    if (info->_subs[i]._checked != Qt::Unchecked) {
                        setData(index.child(i, 0), Qt::Unchecked, Qt::CheckStateRole);
                    }
                }
            }

            if (checked == Qt::PartiallyChecked) {
                QModelIndex parent = index.parent();
                auto parentInfo = infoForIndex(parent);
                if (parentInfo && parentInfo->_checked != Qt::PartiallyChecked) {
                    setData(parent, Qt::PartiallyChecked, Qt::CheckStateRole);
                }
            }

        }
        _dirty = true;
        emit dirtyChanged();
        emit dataChanged(index, index, QVector<int>() << role);
        return true;
    }
    return QAbstractItemModel::setData(index, value, role);
}


int FolderStatusModel::columnCount(const QModelIndex&) const
{
    return 1;
}

int FolderStatusModel::rowCount(const QModelIndex& parent) const
{
    if (!parent.isValid()) {
        if( Theme::instance()->singleSyncFolder() &&  _folders.count() != 0) {
            // "Add folder" button not visible in the singleSyncFolder configuration.
            return _folders.count();
        }
        return _folders.count() + 1; // +1 for the "add folder" button
    }
    auto info = infoForIndex(parent);
    if (!info)
        return 0;
    if (info->hasLabel())
        return 1;
    return info->_subs.count();
}

FolderStatusModel::ItemType FolderStatusModel::classify(const QModelIndex& index) const
{
    if (auto sub = static_cast<SubFolderInfo*>(index.internalPointer())) {
        if (sub->hasLabel()) {
            return FetchLabel;
        } else {
            return SubFolder;
        }
    }
    if (index.row() < _folders.count()) {
        return RootFolder;
    }
    return AddButton;
}

FolderStatusModel::SubFolderInfo* FolderStatusModel::infoForIndex(const QModelIndex& index) const
{
    if (!index.isValid())
        return 0;
    if (auto parentInfo = static_cast<SubFolderInfo*>(index.internalPointer())) {
        if (parentInfo->hasLabel()) {
            return 0;
        }
        return &parentInfo->_subs[index.row()];
    } else {
        if (index.row() >= _folders.count()) {
            // AddButton
            return 0;
        }
        return const_cast<SubFolderInfo *>(&_folders[index.row()]);
    }
}

QModelIndex FolderStatusModel::indexForPath(Folder *f, const QString& path) const
{
    if( !f ) {
        return QModelIndex();
    }

    int slashPos = path.lastIndexOf('/');
    if (slashPos == -1) {
        // first level folder
        for (int i = 0; i < _folders.size(); ++i) {
            if (_folders.at(i)._folder == f) {
                if( path.isEmpty() ) { // the folder object
                    return index(i, 0);
                }
                for (int j = 0; j < _folders.at(i)._subs.size(); ++j) {
                    const QString subName = _folders.at(i)._subs.at(j)._name;
                    if (subName == path) {
                        return index(j, 0, index(i));
                    }
                }
                return QModelIndex();
            }
        }
        return QModelIndex();
    }

    auto parent = indexForPath(f, path.left(slashPos));
    if (!parent.isValid())
        return parent;

    if (slashPos == path.size() - 1) {
        // The slash is the last part, we found our index
        return parent;
    }

    auto parentInfo = infoForIndex(parent);
    if (!parentInfo) {
        return QModelIndex();
    }
    for (int i = 0; i < parentInfo->_subs.size(); ++i) {
        if (parentInfo->_subs.at(i)._name == path.mid(slashPos  + 1)) {
            return index(i, 0, parent);
        }
    }

    return QModelIndex();
}

QModelIndex FolderStatusModel::index(int row, int column, const QModelIndex& parent) const
{
    if (!parent.isValid()) {
        return createIndex(row, column/*, nullptr*/);
    }
    switch(classify(parent)) {
        case AddButton:
        case FetchLabel:
            return QModelIndex();
        case RootFolder:
            if (_folders.count() <= parent.row())
                return QModelIndex(); // should not happen
            return createIndex(row, column, const_cast<SubFolderInfo *>(&_folders[parent.row()]));
        case SubFolder: {
            auto pinfo = static_cast<SubFolderInfo*>(parent.internalPointer());
            if (pinfo->_subs.count() <= parent.row())
                return QModelIndex(); // should not happen
            auto & info = pinfo->_subs[parent.row()];
            if (!info.hasLabel()
                    && info._subs.count() <= row)
                return QModelIndex(); // should not happen
            return createIndex(row, column, &info);
        }
    }
    return QModelIndex();
}

QModelIndex FolderStatusModel::parent(const QModelIndex& child) const
{
    if (!child.isValid()) {
        return QModelIndex();
    }
    switch(classify(child)) {
        case RootFolder:
        case AddButton:
            return QModelIndex();
        case SubFolder:
        case FetchLabel:
            break;
    }
    auto pathIdx = static_cast<SubFolderInfo*>(child.internalPointer())->_pathIdx;
    int i = 1;
    Q_ASSERT(pathIdx.at(0) < _folders.count());
    if (pathIdx.count() == 1) {
        return createIndex(pathIdx.at(0), 0/*, nullptr*/);
    }

    const SubFolderInfo *info = &_folders[pathIdx.at(0)];
    while (i < pathIdx.count() - 1) {
        Q_ASSERT(pathIdx.at(i) < info->_subs.count());
        info = &info->_subs[pathIdx.at(i)];
        ++i;
    }
    return createIndex(pathIdx.at(i), 0, const_cast<SubFolderInfo *>(info));
}

bool FolderStatusModel::hasChildren(const QModelIndex& parent) const
{
    if (!parent.isValid())
        return true;

    auto info = infoForIndex(parent);
    if (!info)
        return false;

    if (!info->_fetched)
        return true;

    if (info->_subs.isEmpty())
        return false;

    return true;
}


bool FolderStatusModel::canFetchMore(const QModelIndex& parent) const
{
    if (!_accountState) {
		return false;
	}
    if (_accountState->state() != AccountState::Connected) {
        return false;
    }
    auto info = infoForIndex(parent);
    if (!info || info->_fetched || info->_fetching)
        return false;
    return true;
}


void FolderStatusModel::fetchMore(const QModelIndex& parent)
{
    auto info = infoForIndex(parent);

    if (!info || info->_fetched || info->_fetching)
        return;

    info->_hasError = false;
    info->_fetching = true;
    info->_fetchingLabel = false;
    QString path = info->_folder->remotePath();
    if (info->_path != QLatin1String("/")) {
        if (!path.endsWith(QLatin1Char('/'))) {
            path += QLatin1Char('/');
        }
        path += info->_path;
    }
    LsColJob *job = new LsColJob(_accountState->account(), path, this);
    job->setProperties(QList<QByteArray>() << "resourcetype" << "http://owncloud.org/ns:size");
    job->setTimeout(60 * 1000);
    connect(job, SIGNAL(directoryListingSubfolders(QStringList)),
            SLOT(slotUpdateDirectories(QStringList)));
    connect(job, SIGNAL(finishedWithError(QNetworkReply*)),
            this, SLOT(slotLscolFinishedWithError(QNetworkReply*)));
    job->start();

    QPersistentModelIndex persistentIndex(parent);
    job->setProperty(propertyParentIndexC , QVariant::fromValue(persistentIndex));

    // Show 'fetching data...' hint after a while.
    _fetchingItems[persistentIndex].start();
    QTimer::singleShot(1000, this, SLOT(slotShowFetchProgress()));
}

void FolderStatusModel::slotUpdateDirectories(const QStringList &list)
{
    auto job = qobject_cast<LsColJob *>(sender());
    Q_ASSERT(job);
    QModelIndex idx = qvariant_cast<QPersistentModelIndex>(job->property(propertyParentIndexC));
    auto parentInfo = infoForIndex(idx);
    if (!parentInfo) {
        return;
    }

    if (parentInfo->hasLabel()) {
        beginRemoveRows(idx, 0 ,0);
        parentInfo->_hasError = false;
        parentInfo->_fetchingLabel = false;
        endRemoveRows();
    }

    parentInfo->_fetching = false;
    parentInfo->_fetched = true;

    QUrl url = parentInfo->_folder->remoteUrl();
    QString pathToRemove = url.path();
    if (!pathToRemove.endsWith('/'))
        pathToRemove += '/';

    QStringList selectiveSyncBlackList;
    if (parentInfo->_checked == Qt::PartiallyChecked) {
        selectiveSyncBlackList = parentInfo->_folder->journalDb()->getSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList);
    }
    auto selectiveSyncUndecidedList = parentInfo->_folder->journalDb()->getSelectiveSyncList(SyncJournalDb::SelectiveSyncUndecidedList);
    QVarLengthArray<int, 10> undecidedIndexes;
    QVector<SubFolderInfo> newSubs;

    std::set<QString> selectiveSyncUndecidedSet; // not QSet because it's not sorted
    foreach (const QString &str, selectiveSyncUndecidedList) {
        if (str.startsWith(parentInfo->_path) || parentInfo->_path == QLatin1String("/")) {
            selectiveSyncUndecidedSet.insert(str);
        }
    }

    newSubs.reserve(list.size() - 1);
    for (int i = 1;  // skip the parent item (first in the list)
            i < list.size(); ++i) {
        const QString &path = list.at(i);
        auto relativePath = path.mid(pathToRemove.size());
        if (parentInfo->_folder->isFileExcludedRelative(relativePath)) {
            continue;
        }

        SubFolderInfo newInfo;
        newInfo._folder = parentInfo->_folder;
        newInfo._pathIdx = parentInfo->_pathIdx;
        newInfo._pathIdx << newSubs.size();
        auto size = job ? job->_sizes.value(path) : 0;
        newInfo._size = size;
        newInfo._path = relativePath;
        newInfo._name = relativePath.split('/', QString::SkipEmptyParts).last();

        if (relativePath.isEmpty())
            continue;

        if (parentInfo->_checked == Qt::Unchecked) {
            newInfo._checked = Qt::Unchecked;
        } else if (parentInfo->_checked == Qt::Checked) {
            newInfo._checked = Qt::Checked;
        } else {
            foreach(const QString &str , selectiveSyncBlackList) {
                if (str == relativePath || str == QLatin1String("/")) {
                    newInfo._checked = Qt::Unchecked;
                    break;
                } else if (str.startsWith(relativePath)) {
                    newInfo._checked = Qt::PartiallyChecked;
                }
            }
        }

        auto it = selectiveSyncUndecidedSet.lower_bound(relativePath);
        if (it != selectiveSyncUndecidedSet.end()) {
            if (*it == relativePath) {
                newInfo._isUndecided = true;
                selectiveSyncUndecidedSet.erase(it);
            } else if ((*it).startsWith(relativePath)) {
                undecidedIndexes.append(newInfo._pathIdx.last());

                // Remove all the items from the selectiveSyncUndecidedSet that starts with this path
                QString relativePathNext = relativePath;
                relativePathNext[relativePathNext.length()-1].unicode()++;
                auto it2 = selectiveSyncUndecidedSet.lower_bound(relativePathNext);
                selectiveSyncUndecidedSet.erase(it, it2);
            }
        }
        newSubs.append(newInfo);
    }

    beginInsertRows(idx, 0, newSubs.size() - 1);
    parentInfo->_subs = std::move(newSubs);
    endInsertRows();

    for (auto it = undecidedIndexes.begin(); it != undecidedIndexes.end(); ++it) {
        suggestExpand(idx.child(*it, 0));
    }

    /* We need lambda function for the following code.
     * It's just a small feature that will be missing if the comiler is too old */
#if !(defined(Q_CC_GNU) && !defined(Q_CC_INTEL) && !defined(Q_CC_CLANG)) || (__GNUC__ * 100 + __GNUC_MINOR__ >= 405)

    /* Try to remove the the undecided lists the items that are not on the server. */
    auto it = std::remove_if(selectiveSyncUndecidedList.begin(), selectiveSyncUndecidedList.end(),
            [&](const QString &s) { return selectiveSyncUndecidedSet.count(s); } );
    if (it != selectiveSyncUndecidedList.end()) {
        selectiveSyncUndecidedList.erase(it, selectiveSyncUndecidedList.end());
        parentInfo->_folder->journalDb()->setSelectiveSyncList(
                            SyncJournalDb::SelectiveSyncUndecidedList, selectiveSyncUndecidedList);
        emit dirtyChanged();
    }
#endif
}

void FolderStatusModel::slotLscolFinishedWithError(QNetworkReply* r)
{
    auto job = qobject_cast<LsColJob *>(sender());
    Q_ASSERT(job);
    QModelIndex idx = qvariant_cast<QPersistentModelIndex>(job->property(propertyParentIndexC));
    if (!idx.isValid()) {
        return;
    }
    auto parentInfo = infoForIndex(idx);
    if (parentInfo) {
        if (r->error() == QNetworkReply::ContentNotFoundError) {
            parentInfo->_fetched = true;
        } else {
            if (!parentInfo->hasLabel()) {
                beginInsertRows(idx, 0, 0);
                endInsertRows();
            }
            parentInfo->_hasError = true;
        }
        parentInfo->_fetching = false;
        parentInfo->_fetchingLabel = false;
    }
}

QStringList FolderStatusModel::createBlackList(FolderStatusModel::SubFolderInfo *root,
                                               const QStringList &oldBlackList) const
{
    if (!root) return QStringList();

    switch(root->_checked) {
        case Qt::Unchecked:
            return QStringList(root->_path);
        case  Qt::Checked:
            return QStringList();
        case Qt::PartiallyChecked:
            break;
    }

    QStringList result;
    if (root->_fetched) {
        for (int i = 0; i < root->_subs.count(); ++i) {
            result += createBlackList(&root->_subs[i], oldBlackList);
        }
    } else {
        // We did not load from the server so we re-use the one from the old black list
        QString path = root->_path;
        foreach (const QString & it, oldBlackList) {
            if (it.startsWith(path))
                result += it;
        }
    }
    return result;
}

void FolderStatusModel::slotUpdateFolderState(Folder *folder)
{
    if( ! folder ) return;
    for (int i = 0; i < _folders.count(); ++i) {
        if (_folders.at(i)._folder == folder) {
            emit dataChanged(index(i), index(i));
        }
    }
}

void FolderStatusModel::slotApplySelectiveSync()
{
    for (int i = 0; i < _folders.count(); ++i) {
        if (!_folders[i]._fetched) {
            _folders[i]._folder->journalDb()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncUndecidedList, QStringList());
            continue;
        }
        auto folder = _folders.at(i)._folder;

        auto oldBlackList = folder->journalDb()->getSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList);
        QStringList blackList = createBlackList(&_folders[i], oldBlackList);
        folder->journalDb()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, blackList);

        auto blackListSet = blackList.toSet();
        auto oldBlackListSet = oldBlackList.toSet();

        // The folders that were undecided or blacklisted and that are now checked should go on the white list.
        // The user confirmed them already just now.
        QStringList toAddToWhiteList = ((oldBlackListSet + folder->journalDb()->getSelectiveSyncList(
                SyncJournalDb::SelectiveSyncUndecidedList).toSet()) - blackListSet).toList();

        if (!toAddToWhiteList.isEmpty()) {
            auto whiteList = folder->journalDb()->getSelectiveSyncList(SyncJournalDb::SelectiveSyncWhiteList);
            whiteList += toAddToWhiteList;
            folder->journalDb()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncWhiteList, whiteList);
        }
        // clear the undecided list
        folder->journalDb()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncUndecidedList, QStringList());

        // do the sync if there were changes
        auto changes = (oldBlackListSet - blackListSet) + (blackListSet - oldBlackListSet);
        if (!changes.isEmpty()) {
            if (folder->isBusy()) {
                folder->slotTerminateSync();
            }
            //The part that changed should not be read from the DB on next sync because there might be new folders
            // (the ones that are no longer in the blacklist)
            foreach(const auto &it, changes) {
                folder->journalDb()->avoidReadFromDbOnNextSync(it);
            }
            FolderMan::instance()->slotScheduleSync(folder);
        }
    }

    resetFolders();
}

static QString shortenFilename( Folder *f, const QString& file )
{
    // strip off the server prefix from the file name
    QString shortFile(file);
    if( shortFile.isEmpty() ) {
        return QString::null;
    }

    if(shortFile.startsWith(QLatin1String("ownclouds://")) ||
            shortFile.startsWith(QLatin1String("owncloud://")) ) {
        // rip off the whole ownCloud URL.
        if( f ) {
            QString remotePathUrl = f->remoteUrl().toString();
            shortFile.remove(Utility::toCSyncScheme(remotePathUrl));
        }
    }
    return shortFile;
}

void FolderStatusModel::slotSetProgress(const ProgressInfo &progress)
{
    auto par = qobject_cast<QWidget*>(QObject::parent());
    if (!par->isVisible()) {
        return; // for https://github.com/owncloud/client/issues/2648#issuecomment-71377909
    }

    Folder *f = qobject_cast<Folder*>(sender());
    if( !f ) { return; }

    int folderIndex = -1;
    for (int i = 0; i < _folders.count(); ++i) {
        if (_folders.at(i)._folder == f) {
            folderIndex = i;
            break;
        }
    }
    if (folderIndex < 0) { return; }

    auto *pi = &_folders[folderIndex]._progress;

    QVector<int> roles;
    roles << FolderStatusDelegate::SyncProgressItemString << FolderStatusDelegate::WarningCount;

    if (!progress._currentDiscoveredFolder.isEmpty()) {
        pi->_progressString = tr("Checking for changes in '%1'").arg(progress._currentDiscoveredFolder);
        emit dataChanged(index(folderIndex), index(folderIndex), roles);
        return;
    }

    if(!progress._lastCompletedItem.isEmpty()
            && Progress::isWarningKind(progress._lastCompletedItem._status)) {
        pi->_warningCount++;
    }

    // find the single item to display:  This is going to be the bigger item, or the last completed
    // item if no items are in progress.
    SyncFileItem curItem = progress._lastCompletedItem;
    qint64 curItemProgress = -1; // -1 means finished
    quint64 biggerItemSize = -1;
    quint64 estimatedUpBw = 0;
    quint64 estimatedDownBw = 0;
    QString allFilenames;
    foreach(const ProgressInfo::ProgressItem &citm, progress._currentItems) {
        if (curItemProgress == -1 || (ProgressInfo::isSizeDependent(citm._item)
                                      && biggerItemSize < citm._item._size)) {
            curItemProgress = citm._progress.completed();
            curItem = citm._item;
            biggerItemSize = citm._item._size;
        }
        if (citm._item._direction != SyncFileItem::Up){
            estimatedDownBw += progress.fileProgress(citm._item).estimatedBandwidth;
            //qDebug() << "DOWN" << citm._item._file << progress.fileProgress(citm._item).estimatedBandwidth;
        } else {
            estimatedUpBw += progress.fileProgress(citm._item).estimatedBandwidth;
            //qDebug() << "UP" << citm._item._file << progress.fileProgress(citm._item).estimatedBandwidth;
        }
        auto fileName = QFileInfo(citm._item._file).fileName();
        if (allFilenames.length() > 0) {
            //: Build a list of file names
            allFilenames.append(tr(", '%1'").arg(fileName));
        } else {
            //: Argument is a file name
            allFilenames.append(tr("'%1'").arg(fileName));
        }
    }
    //qDebug() << "Syncing bandwidth" << estimatedDownBw << estimatedUpBw;
    if (curItemProgress == -1) {
        curItemProgress = curItem._size;
    }

    QString itemFileName = shortenFilename(f, curItem._file);
    QString kindString = Progress::asActionString(curItem);

    QString fileProgressString;
    if (ProgressInfo::isSizeDependent(curItem)) {
        QString s1 = Utility::octetsToString( curItemProgress );
        QString s2 = Utility::octetsToString( curItem._size );
        //quint64 estimatedBw = progress.fileProgress(curItem).estimatedBandwidth;
        if (estimatedUpBw || estimatedDownBw) {
            /*
            //: Example text: "uploading foobar.png (1MB of 2MB) time left 2 minutes at a rate of 24Kb/s"
            fileProgressString = tr("%1 %2 (%3 of %4) %5 left at a rate of %6/s")
                .arg(kindString, itemFileName, s1, s2,
                    Utility::durationToDescriptiveString(progress.fileProgress(curItem).estimatedEta),
                    Utility::octetsToString(estimatedBw) );
            */
            //: Example text: "Syncing 'foo.txt', 'bar.txt'"
            fileProgressString = tr("Syncing %1").arg(allFilenames);
            if (estimatedDownBw > 0) {
                fileProgressString.append(tr(", "));
// ifdefs: https://github.com/owncloud/client/issues/3095#issuecomment-128409294
#ifdef Q_OS_WIN
                //: Example text: "download 24Kb/s"   (%1 is replaced by 24Kb (translated))
                fileProgressString.append(tr("download %1/s").arg(Utility::octetsToString(estimatedDownBw)));
#else
                fileProgressString.append(trUtf8("\u2193" " %1/s").arg(Utility::octetsToString(estimatedDownBw)));
#endif
            }
            if (estimatedUpBw > 0) {
                fileProgressString.append(tr(", "));
 #ifdef Q_OS_WIN
                //: Example text: "upload 24Kb/s"   (%1 is replaced by 24Kb (translated))
                fileProgressString.append(tr("upload %1/s").arg(Utility::octetsToString(estimatedUpBw)));
#else
                fileProgressString.append(trUtf8("\u2191" " %1/s").arg(Utility::octetsToString(estimatedUpBw)));
#endif
            }
        } else {
            //: Example text: "uploading foobar.png (2MB of 2MB)"
            fileProgressString = tr("%1 %2 (%3 of %4)") .arg(kindString, itemFileName, s1, s2);
        }
    } else if (!kindString.isEmpty()) {
        //: Example text: "uploading foobar.png"
        fileProgressString = tr("%1 %2").arg(kindString, itemFileName);
    }
    pi->_progressString = fileProgressString;

    // overall progress
    quint64 completedSize = progress.completedSize();
    quint64 completedFile = progress.completedFiles();
    quint64 currentFile = progress.currentFile();
    if (currentFile == ULLONG_MAX)
        currentFile = 0;
    quint64 totalSize = qMax(completedSize, progress.totalSize());
    quint64 totalFileCount = qMax(currentFile, progress.totalFiles());
    QString overallSyncString;
    if (totalSize > 0) {
        QString s1 = Utility::octetsToString( completedSize );
        QString s2 = Utility::octetsToString( totalSize );
        //: Example text: "12 MB of 345 MB, file 6 of 7\nTotal time left 12 minutes"
        overallSyncString = tr("%1 of %2, file %3 of %4\nTotal time left %5")
            .arg(s1, s2)
            .arg(currentFile).arg(totalFileCount)
            .arg( Utility::durationToDescriptiveString(progress.totalProgress().estimatedEta) );
    } else if (totalFileCount > 0) {
        // Don't attempt to estimate the time left if there is no kb to transfer.
        overallSyncString = tr("file %1 of %2") .arg(currentFile).arg(totalFileCount);
    }

    pi->_overallSyncString =  overallSyncString;

    int overallPercent = 0;
    if( totalFileCount > 0 ) {
        // Add one 'byte' for each file so the percentage is moving when deleting or renaming files
        overallPercent = qRound(double(completedSize + completedFile)/double(totalSize + totalFileCount) * 100.0);
    }
    pi->_overallPercent = qBound(0, overallPercent, 100);
    emit dataChanged(index(folderIndex), index(folderIndex), roles);
}

void FolderStatusModel::slotFolderSyncStateChange(Folder *f)
{
    if( !f ) { return; }

    int folderIndex = -1;
    for (int i = 0; i < _folders.count(); ++i) {
        if (_folders.at(i)._folder == f) {
            folderIndex = i;
            break;
        }
    }
    if (folderIndex < 0) { return; }

    SyncResult::Status state = f->syncResult().status();
    if (f->syncPaused()) {
        // Reset progress info.
        _folders[folderIndex]._progress = SubFolderInfo::Progress();
    } else if (state == SyncResult::NotYetStarted) {
        FolderMan* folderMan = FolderMan::instance();
        int pos = folderMan->scheduleQueue().indexOf(f);
        if (folderMan->currentSyncFolder()
                && folderMan->currentSyncFolder() != f) {
            pos += 1;
        }
        QString message;
        if (pos <= 0) {
            message = tr("Waiting...");
        } else {
            message = tr("Waiting for %n other folder(s)...", "", pos);
        }
        _folders[folderIndex]._progress = SubFolderInfo::Progress();
        _folders[folderIndex]._progress._progressString = message;
    } else if (state == SyncResult::SyncPrepare) {
        _folders[folderIndex]._progress = SubFolderInfo::Progress();
        _folders[folderIndex]._progress._progressString = tr("Preparing to sync...");
    } else if (state == SyncResult::Problem || state == SyncResult::Success) {
        // Reset the progress info after a sync.
        _folders[folderIndex]._progress = SubFolderInfo::Progress();
    } else if (state == SyncResult::Error) {
        _folders[folderIndex]._progress = SubFolderInfo::Progress();
    }

    // update the icon etc. now
    slotUpdateFolderState(f);

    if (state == SyncResult::Success) {
        foreach (const SyncFileItemPtr &i, f->syncResult().syncFileItemVector()) {
            if (i->_isDirectory && (i->_instruction == CSYNC_INSTRUCTION_NEW
                    || i->_instruction == CSYNC_INSTRUCTION_TYPE_CHANGE
                    || i->_instruction == CSYNC_INSTRUCTION_REMOVE
                    || i->_instruction == CSYNC_INSTRUCTION_RENAME)) {
                // There is a new or a removed folder. reset all data
                auto & info = _folders[folderIndex];
                info.resetSubs(this, index(folderIndex));
                return;
            }
        }
    }
}

void FolderStatusModel::slotFolderScheduleQueueChanged()
{
    // Update messages on waiting folders.
    // It's ok to only update folders currently in the queue, because folders
    // are only removed from the queue if they are deleted.
    foreach (Folder* f, FolderMan::instance()->scheduleQueue()) {
        slotFolderSyncStateChange(f);
    }
}

void FolderStatusModel::resetFolders()
{
    setAccountState(_accountState);
}

void FolderStatusModel::slotNewBigFolder()
{
    auto f = qobject_cast<Folder *>(sender());
    Q_ASSERT(f);

    int folderIndex = -1;
    for (int i = 0; i < _folders.count(); ++i) {
        if (_folders.at(i)._folder == f) {
            folderIndex = i;
            break;
        }
    }
    if (folderIndex < 0) { return; }

    _folders[folderIndex].resetSubs(this, index(folderIndex));

    emit suggestExpand(index(folderIndex));
    emit dirtyChanged();
}

void FolderStatusModel::slotShowFetchProgress()
{
    QMutableMapIterator<QPersistentModelIndex, QElapsedTimer> it(_fetchingItems);
    while (it.hasNext()) {
        it.next();
        if (it.value().elapsed() > 800)
        {
            auto idx = it.key();
            auto* info = infoForIndex(idx);
            if (info && info->_fetching) {
                if (!info->hasLabel()) {
                    beginInsertRows(idx, 0, 0);
                    endInsertRows();
                }
                info->_fetchingLabel = true;
            }
            it.remove();
        }
    }
}

bool FolderStatusModel::SubFolderInfo::hasLabel() const
{
    return _hasError || _fetchingLabel;
}

void FolderStatusModel::SubFolderInfo::resetSubs(FolderStatusModel* model, QModelIndex index)
{
    _fetched = false;
    _fetching = false;
    if (hasLabel()) {
        model->beginRemoveRows(index, 0 ,0);
        _fetchingLabel = false;
        _hasError = false;
        model->endRemoveRows();
    } else if (!_subs.isEmpty()) {
        model->beginRemoveRows(index, 0, _subs.count() - 1);
        _subs.clear();
        model->endRemoveRows();
    }
}


} // namespace OCC

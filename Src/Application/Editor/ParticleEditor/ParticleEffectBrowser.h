#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

//=============================================================
// ParticleEffectBrowser
// ParticleEditor のエフェクト一覧(名前・タグでの絞り込み)と、開いているエフェクトのタグ編集UI。
// 一覧と絞り込みは保存済みの内容(JSON)で動く。開いているエフェクトの名前・タグ・未保存かは
// ParticleEditor の状態なので毎フレーム ViewState で受け取り、開く・タグの変更は Result で返す
// (開く処理・Undo・タグの書き換えは ParticleEditor が行う)
//=============================================================
class ParticleEffectBrowser
{
public:

	// ParticleEditor が今開いているエフェクトの、一覧の表示に必要な状態
	struct ViewState
	{
		const std::string& curName;						// 開いているエフェクトの名前
		const std::vector<std::string>& curTags;		// 開いているエフェクトのタグ
		bool hasUnsavedChanges = false;					// 保存していない編集があるか
	};

	// 1フレームの操作の結果
	struct Result
	{
		std::string openName;		// 一覧で選ばれた名前(空なら無し)
		int removeTagIdx = -1;		// 外すタグの位置(無ければ -1)
		std::string addTag;			// 加えるタグ(前後の空白を除き、空・重複は除いた後。空なら無し)
	};

	// 検索欄・タグ絞り込み・一覧・開いているエフェクトのタグ編集をまとめて描く
	Result Draw(const ViewState& a_view);

	// 指定の JSON から登録済みの名前とタグの一覧を取得し直す
	void Refresh(const std::string& a_jsonName);

	// 一覧に出る名前(名前順)
	const std::vector<std::string>& GetNames() const { return m_emitterNames; }

	// 一覧に出さないv1だけの定義も含め、JSON に登録されている名前か(新しい名前の重なりを確かめる用)
	bool IsRegistered(const std::string& a_name) const;

private:

	// 保存済みタグの絞り込みボタン群
	void DrawBrowserTagFilter();

	// 絞り込み条件に合うエフェクトの一覧。クリックされた名前を返す(無ければ空)
	std::string DrawBrowserEntryList(const ViewState& a_view);

	// 開いているエフェクトのタグ追加・削除UI。変更は a_result へ入れる
	void DrawCurTagEditor(const ViewState& a_view, Result& a_result);

	// 付いているタグの一覧と、押したタグを外す操作。外すタグの位置を返す(無ければ -1)
	int DrawTagChips(const std::vector<std::string>& a_tags);

	// 表記揺れを防ぐため、既に使われているタグから選んで加える。選んだタグを返す(無ければ空)
	std::string DrawExistingTagPicker(const std::vector<std::string>& a_tags);

	// 検索語(小文字化済み)・選択タグ・タグなし指定のすべてを満たすか
	bool IsBrowserEntryVisible(
		const std::string& a_name,
		const std::vector<std::string>& a_tags,
		const std::vector<std::string>& a_searchTerms) const;

	// タグを加える要求を result へ入れる(前後の空白を除き、空・重複は無視する)
	void RequestAddTag(const std::string& a_tag, const std::vector<std::string>& a_curTags, Result& a_result) const;

	// 現在の JSON に登録されているエミッター名一覧(エフェクト一覧の表示用)
	std::vector<std::string> m_emitterNames;

	// 一覧に出さないv1だけの定義も含めた、JSON に登録されている全ての名前
	std::unordered_set<std::string> m_registeredNames;

	// 保存済みのエフェクト名→タグ。一覧の表示と絞り込みはファイルの内容を基準にする
	std::unordered_map<std::string, std::vector<std::string>> m_savedTagsByName;

	// 保存済みタグと付いている件数(件数の多い順)。絞り込みボタンの並びに使う
	std::vector<std::pair<std::string, int>> m_savedTagCnts;

	// 一覧の検索語(名前とタグの部分一致。空白区切りはすべてを含むもの)
	char m_searchBuf[128] = "";

	// 絞り込みに使うタグ(すべてを持つものだけ表示する)
	std::vector<std::string> m_tagFilter;

	// タグが1つも無いものだけを表示する(分類漏れの確認用)
	bool m_isUntaggedOnly = false;

	// タグ追加欄の入力バッファ
	char m_tagInputBuf[64] = "";
};

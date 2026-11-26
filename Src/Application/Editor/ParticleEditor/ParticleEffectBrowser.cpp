//=============================================================
// ParticleEffectBrowser
// エフェクト一覧(名前・タグでの絞り込み)と、開いているエフェクトのタグ編集UIの実装。
//=============================================================

#include "Application/Editor/ParticleEditor/ParticleEffectBrowser.h"
#include "Application/Effect/Particle/Serialization/ParticleSerializer.h"
#include "Application/Editor/Common/EditorMenu.h"
#include "Application/Editor/ParticleEditor/ParticleEditorConst.h"
#include "Application/System/Utility/StringUtil.h"


namespace
{
	// 全角空白を半角へ寄せる
	std::string NormalizeSpaces(std::string a_text)
	{
		const std::string_view fullWidthSpace = ParticleEditorConst::FULL_WIDTH_SPACE;

		for (size_t pos = a_text.find(fullWidthSpace); pos != std::string::npos; pos = a_text.find(fullWidthSpace, pos))
		{
			a_text.replace(pos, fullWidthSpace.size(), " ");
		}
		return a_text;
	}

	// 前後の空白(半角・全角・タブ)を除く
	std::string TrimSpaces(const std::string& a_text)
	{
		const std::string normalized = NormalizeSpaces(a_text);
		const size_t first = normalized.find_first_not_of(" \t");
		if (first == std::string::npos)
		{
			return {};
		}

		const size_t last = normalized.find_last_not_of(" \t");
		return normalized.substr(first, last - first + 1);
	}

	// 検索欄の文字列を空白で区切り、小文字化した語の一覧にする
	std::vector<std::string> SplitSearchTerms(const char* a_query)
	{
		const std::string lowerQuery = StringUtil::ToLowerAscii(NormalizeSpaces(a_query));
		std::istringstream stream(lowerQuery);

		std::vector<std::string> terms;
		std::string term;
		while (stream >> term)
		{
			terms.push_back(term);
		}
		return terms;
	}

	bool ContainsTag(const std::vector<std::string>& a_tags, const std::string& a_tag)
	{
		return std::find(a_tags.begin(), a_tags.end(), a_tag) != a_tags.end();
	}

	std::string JoinTags(const std::vector<std::string>& a_tags)
	{
		std::string joined;
		for (const std::string& tag : a_tags)
		{
			if (!joined.empty())
			{
				joined += ", ";
			}
			joined += tag;
		}
		return joined;
	}

	// 小さなボタンを横へ並べ、行の右端 rightEdge を超える時だけ改行する。
	// isHighlighted のものはボタンを押下色で塗り、選択中であることを示す
	bool WrappedChip(const char* a_label, bool a_isHighlighted, bool a_isFirst, float a_rightEdge)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const float width = ImGui::CalcTextSize(a_label, nullptr, true).x + style.FramePadding.x * 2.0f;

		if (!a_isFirst && ImGui::GetItemRectMax().x + style.ItemSpacing.x + width <= a_rightEdge)
		{
			ImGui::SameLine();
		}

		if (a_isHighlighted)
		{
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
		}

		const bool isPressed = ImGui::SmallButton(a_label);

		if (a_isHighlighted)
		{
			ImGui::PopStyleColor();
		}

		return isPressed;
	}
}

//=============================================================
// Draw
// 100件を超えるエフェクトから目的のものを探せるよう、名前とタグで絞り込んで開く
//=============================================================
ParticleEffectBrowser::Result ParticleEffectBrowser::Draw(const ViewState& a_view)
{
	Result result;

	EditorMenu::SectionHeading(U8("エフェクト一覧"));

	const bool hasCondition =
		(m_searchBuf[0] != '\0')
		|| !m_tagFilter.empty()
		|| m_isUntaggedOnly;

	const float clearBtnWidth =
		ImGui::CalcTextSize(U8("条件を解除")).x
		+ ImGui::GetStyle().FramePadding.x * 2.0f
		+ ImGui::GetStyle().ItemSpacing.x;

	ImGui::SetNextItemWidth(-clearBtnWidth);
	ImGui::InputTextWithHint(
		"##browserSearch",
		U8("名前・タグで検索（空白区切りで複数語）"),
		m_searchBuf,
		sizeof(m_searchBuf));

	ImGui::SameLine();

	ImGui::BeginDisabled(!hasCondition);
	if (ImGui::Button(U8("条件を解除")))
	{
		m_searchBuf[0] = '\0';
		m_tagFilter.clear();
		m_isUntaggedOnly = false;
	}
	ImGui::EndDisabled();

	DrawBrowserTagFilter();
	result.openName = DrawBrowserEntryList(a_view);

	ImGui::Spacing();
	DrawCurTagEditor(a_view, result);

	return result;
}

//=============================================================
// DrawBrowserTagFilter
// 押したタグをすべて持つものだけに絞る。もう一度押すと外れる
//=============================================================
void ParticleEffectBrowser::DrawBrowserTagFilter()
{
	if (m_savedTagCnts.empty())
	{
		ImGui::TextDisabled(U8("(タグの付いたエフェクトはまだありません)"));
		return;
	}

	const float rightEdge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;

	bool isFirst = true;
	for (const auto& [tag, count] : m_savedTagCnts)
	{
		const bool isActive = ContainsTag(m_tagFilter, tag);
		const std::string label = tag + " (" + std::to_string(count) + ")";

		ImGui::PushID(tag.c_str());
		if (WrappedChip(label.c_str(), isActive, isFirst, rightEdge))
		{
			if (isActive)
			{
				std::erase(m_tagFilter, tag);
			}
			else
			{
				m_tagFilter.push_back(tag);
				m_isUntaggedOnly = false;
			}
		}
		ImGui::PopID();

		isFirst = false;
	}

	// タグ付けの漏れを探すための特別な絞り込み。タグ指定とは両立しないため切り替える
	if (WrappedChip(U8("タグなし##untagged"), m_isUntaggedOnly, false, rightEdge))
	{
		m_isUntaggedOnly = !m_isUntaggedOnly;
		if (m_isUntaggedOnly)
		{
			m_tagFilter.clear();
		}
	}
}

//=============================================================
// DrawBrowserEntryList
// 名前の右に保存済みのタグを並べる。クリックされた名前を返す(開くのは呼び出し側)
//=============================================================
std::string ParticleEffectBrowser::DrawBrowserEntryList(const ViewState& a_view)
{
	const std::vector<std::string> searchTerms = SplitSearchTerms(m_searchBuf);
	const std::vector<std::string> noTags;

	// タグ列の開始位置を最長の名前に揃え、行ごとに位置がずれないようにする
	float maxNameWidth = 0.0f;
	for (const std::string& name : m_emitterNames)
	{
		maxNameWidth = std::max(maxNameWidth, ImGui::CalcTextSize(name.c_str()).x);
	}

	std::string openName;
	int visibleCnt = 0;

	ImGui::BeginChild("##EffectBrowserList", ImVec2(0.0f, ParticleEditorConst::BROWSER_LIST_HEIGHT), true);

	const float tagColumnX = ImGui::GetCursorPosX() + maxNameWidth + ImGui::GetStyle().ItemSpacing.x * 2.0f;

	for (const std::string& name : m_emitterNames)
	{
		const auto it = m_savedTagsByName.find(name);
		const std::vector<std::string>& tags = (it != m_savedTagsByName.end()) ? it->second : noTags;

		if (!IsBrowserEntryVisible(name, tags, searchTerms))
		{
			continue;
		}

		++visibleCnt;

		ImGui::PushID(name.c_str());

		// 開いているエフェクトに保存していない編集があれば、行にも「*」を付ける
		const bool isCur = (name == a_view.curName);
		const std::string label = (isCur && a_view.hasUnsavedChanges) ? name + " *" : name;

		if (ImGui::Selectable(label.c_str(), isCur))
		{
			openName = name;
		}

		// 狭いウィンドウではタグ列が切れるため、全体はツールチップで見せる
		if (!tags.empty())
		{
			const std::string joined = JoinTags(tags);

			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("%s", joined.c_str());
			}

			ImGui::SameLine(tagColumnX);
			ImGui::TextDisabled("%s", joined.c_str());
		}

		ImGui::PopID();
	}

	if (visibleCnt == 0)
	{
		ImGui::TextDisabled(U8("条件に合うエフェクトがありません"));
	}

	ImGui::EndChild();

	ImGui::TextDisabled(
		U8("%d / %d 件"),
		visibleCnt,
		static_cast<int>(m_emitterNames.size()));

	// 一覧の走査を終えてから開く(読み込みで選択名が変わっても走査に影響させない)ため、名前だけを返す
	return openName;
}

//=============================================================
// DrawTagChips  [private]
// 付いているタグの一覧と、押したタグを外す操作。外すタグの位置を返す(無ければ -1)
//=============================================================
int ParticleEffectBrowser::DrawTagChips(const std::vector<std::string>& a_tags)
{
	// 押したタグを外す。走査中に配列を縮めないよう、位置だけ覚えて後で消す
	const float rightEdge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
	int removeIdx = -1;

	for (int i = 0; i < static_cast<int>(a_tags.size()); ++i)
	{
		const std::string label = a_tags[i] + U8(" ×");

		ImGui::PushID(i);
		if (WrappedChip(label.c_str(), false, i == 0, rightEdge))
		{
			removeIdx = i;
		}
		ImGui::PopID();
	}

	return removeIdx;
}

//=============================================================
// DrawExistingTagPicker  [private]
// 表記揺れを防ぐため、既に使われているタグから選んで加える。選んだタグを返す(無ければ空)
//=============================================================
std::string ParticleEffectBrowser::DrawExistingTagPicker(const std::vector<std::string>& a_tags)
{
	// 表記揺れを防ぐため、既に使われているタグから選べるようにする
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-FLT_MIN);

	std::string pickedTag;
	if (ImGui::BeginCombo("##tagPick", U8("既存タグから")))
	{
		for (const auto& [tag, count] : m_savedTagCnts)
		{
			if (ContainsTag(a_tags, tag))
			{
				continue;
			}

			if (ImGui::Selectable(tag.c_str()))
			{
				pickedTag = tag;
			}
		}

		ImGui::EndCombo();
	}

	return pickedTag;
}

//=============================================================
// DrawCurTagEditor
// 開いているエフェクトのタグを編集する。変更は result へ入れて返すだけで、
// 反映(ツリーと同じくUndo対象、保存時に書き込む)は呼び出し側が行う
//=============================================================
void ParticleEffectBrowser::DrawCurTagEditor(const ViewState& a_view, Result& a_result)
{
	ImGui::SeparatorText(U8("開いているエフェクトのタグ"));
	ImGui::TextUnformatted(a_view.curName.c_str());

	const std::vector<std::string>& tags = a_view.curTags;

	if (tags.empty())
	{
		ImGui::TextDisabled(U8("(タグなし)"));
	}

	a_result.removeTagIdx = DrawTagChips(tags);

	// Enterで追加した後も入力欄に留まり、続けて打てるようにする
	ImGui::SetNextItemWidth(-ParticleEditorConst::TAG_INPUT_RESERVED_WIDTH);
	const bool isEntered = ImGui::InputTextWithHint(
		"##tagInput",
		U8("新しいタグ"),
		m_tagInputBuf,
		sizeof(m_tagInputBuf),
		ImGuiInputTextFlags_EnterReturnsTrue);

	if (isEntered)
	{
		ImGui::SetKeyboardFocusHere(-1);
	}

	ImGui::SameLine();
	const bool isAddPressed = ImGui::Button(U8("追加##tag"));

	if (isEntered || isAddPressed)
	{
		RequestAddTag(m_tagInputBuf, tags, a_result);
		m_tagInputBuf[0] = '\0';
	}

	const std::string pickedTag = DrawExistingTagPicker(tags);
	if (!pickedTag.empty())
	{
		RequestAddTag(pickedTag, tags, a_result);
	}

	// 一覧と絞り込みは保存済みの内容で動くため、未保存のまま別のエフェクトを開くと失われることを知らせる
	const auto it = m_savedTagsByName.find(a_view.curName);
	const bool isUnsaved = (it == m_savedTagsByName.end())
		? !tags.empty()
		: (it->second != tags);

	if (isUnsaved)
	{
		ImGui::TextColored(
			ImVec4(
				ParticleEditorConst::UNSAVED_TEXT_R,
				ParticleEditorConst::UNSAVED_TEXT_G,
				ParticleEditorConst::UNSAVED_TEXT_B,
				1.0f),
			U8("タグは未保存です（Ctrl+S で保存）"));
	}
}

bool ParticleEffectBrowser::IsBrowserEntryVisible(
	const std::string& a_name,
	const std::vector<std::string>& a_tags,
	const std::vector<std::string>& a_searchTerms) const
{
	if (m_isUntaggedOnly && !a_tags.empty())
	{
		return false;
	}

	for (const std::string& filterTag : m_tagFilter)
	{
		if (!ContainsTag(a_tags, filterTag))
		{
			return false;
		}
	}

	if (a_searchTerms.empty())
	{
		return true;
	}

	const std::string lowerName = StringUtil::ToLowerAscii(a_name);

	// 検索語はそれぞれ、名前かいずれかのタグに含まれていればよい
	for (const std::string& term : a_searchTerms)
	{
		if (lowerName.find(term) != std::string::npos)
		{
			continue;
		}

		const bool isMatchedTag = std::any_of(
			a_tags.begin(),
			a_tags.end(),
			[&term](const std::string& a_tag) { return StringUtil::ToLowerAscii(a_tag).find(term) != std::string::npos; });

		if (!isMatchedTag)
		{
			return false;
		}
	}

	return true;
}

void ParticleEffectBrowser::RequestAddTag(
	const std::string& a_tag,
	const std::vector<std::string>& a_curTags,
	Result& a_result) const
{
	const std::string trimmed = TrimSpaces(a_tag);
	if (trimmed.empty() || ContainsTag(a_curTags, trimmed))
	{
		return;
	}

	a_result.addTag = trimmed;
}

//=============================================================
// Refresh
// 指定の JSON から登録済みのエミッター/エフェクト名一覧を取得する
//=============================================================
void ParticleEffectBrowser::Refresh(const std::string& a_jsonName)
{
	m_emitterNames.clear();

	std::unordered_set<std::string> treeNodeNames;
	Particle::ParticleSerializer::LoadEmitterNames(
		a_jsonName,
		m_emitterNames,
		&m_savedTagsByName,
		&treeNodeNames);

	// 新しく付ける名前の重なりは、一覧に出さない定義も含めて確かめるため、絞り込む前に全て覚えておく
	m_registeredNames.clear();
	m_registeredNames.insert(m_emitterNames.begin(), m_emitterNames.end());

	// ツリーの子へ移った後に残った古いv1だけの定義は、ツリー側が正のため一覧に出さない。
	// v2のエフェクトはタグの一覧に名前があるため、そこに無い名前をv1だけの定義とみなす
	std::vector<std::string> filteredNames;
	filteredNames.reserve(m_emitterNames.size());
	for (const std::string& name : m_emitterNames)
	{
		const bool isV1Only = !m_savedTagsByName.contains(name);

		if (isV1Only && treeNodeNames.contains(name))
		{
			continue;
		}

		filteredNames.push_back(name);
	}
	m_emitterNames.swap(filteredNames);

	// 名前は用途の接頭辞(Attack/Boss/Env…)で始まるため、名前順に並べると種類ごとにまとまる
	std::sort(m_emitterNames.begin(), m_emitterNames.end());

	// 一覧に出る名前だけでタグの件数を数え、多い順(同数は名前順)に並べる
	std::unordered_map<std::string, int> tagCnts;
	for (const std::string& name : m_emitterNames)
	{
		const auto it = m_savedTagsByName.find(name);
		if (it == m_savedTagsByName.end())
		{
			continue;
		}

		for (const std::string& tag : it->second)
		{
			++tagCnts[tag];
		}
	}

	m_savedTagCnts.assign(tagCnts.begin(), tagCnts.end());
	std::sort(
		m_savedTagCnts.begin(),
		m_savedTagCnts.end(),
		[](const std::pair<std::string, int>& a_a, const std::pair<std::string, int>& a_b)
		{
			if (a_a.second != a_b.second)
			{
				return a_a.second > a_b.second;
			}
			return a_a.first < a_b.first;
		});

	// ファイルから消えたタグが絞り込みに残ると何も表示されなくなるため外す
	std::erase_if(
		m_tagFilter,
		[&tagCnts](const std::string& a_tag) { return !tagCnts.contains(a_tag); });
}

bool ParticleEffectBrowser::IsRegistered(const std::string& a_name) const
{
	return m_registeredNames.contains(a_name);
}

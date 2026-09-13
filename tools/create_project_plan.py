from pathlib import Path

from docx import Document
from docx.enum.section import WD_SECTION
from docx.enum.table import WD_CELL_VERTICAL_ALIGNMENT, WD_TABLE_ALIGNMENT
from docx.enum.text import WD_ALIGN_PARAGRAPH, WD_BREAK, WD_LINE_SPACING
from docx.oxml import OxmlElement
from docx.oxml.ns import qn
from docx.shared import Cm, Pt, RGBColor


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "deliverables"
OUT_PATH = OUT_DIR / "基于ESP32开发的在线电子阅读器项目计划书.docx"

NAVY = "17365D"
PALE_BLUE = "EAF2F8"
PALE_GRAY = "F5F7F9"
BORDER = "D9D9D9"


def set_run_font(run, east="宋体", western="Arial", size=None, bold=None, color="000000"):
    run.font.name = western
    run._element.get_or_add_rPr().rFonts.set(qn("w:eastAsia"), east)
    run._element.get_or_add_rPr().rFonts.set(qn("w:ascii"), western)
    run._element.get_or_add_rPr().rFonts.set(qn("w:hAnsi"), western)
    if size is not None:
        run.font.size = Pt(size)
    if bold is not None:
        run.bold = bold
    run.font.color.rgb = RGBColor.from_string(color)


def set_cell_shading(cell, fill):
    tc_pr = cell._tc.get_or_add_tcPr()
    shd = tc_pr.find(qn("w:shd"))
    if shd is None:
        shd = OxmlElement("w:shd")
        tc_pr.append(shd)
    shd.set(qn("w:fill"), fill)


def set_cell_margins(cell, top=100, start=120, bottom=100, end=120):
    tc = cell._tc
    tc_pr = tc.get_or_add_tcPr()
    tc_mar = tc_pr.first_child_found_in("w:tcMar")
    if tc_mar is None:
        tc_mar = OxmlElement("w:tcMar")
        tc_pr.append(tc_mar)
    for tag, value in (("top", top), ("start", start), ("bottom", bottom), ("end", end)):
        node = tc_mar.find(qn(f"w:{tag}"))
        if node is None:
            node = OxmlElement(f"w:{tag}")
            tc_mar.append(node)
        node.set(qn("w:w"), str(value))
        node.set(qn("w:type"), "dxa")


def set_table_borders(table, color=BORDER, size="6"):
    tbl_pr = table._tbl.tblPr
    borders = tbl_pr.first_child_found_in("w:tblBorders")
    if borders is None:
        borders = OxmlElement("w:tblBorders")
        tbl_pr.append(borders)
    for edge in ("top", "left", "bottom", "right", "insideH", "insideV"):
        tag = borders.find(qn(f"w:{edge}"))
        if tag is None:
            tag = OxmlElement(f"w:{edge}")
            borders.append(tag)
        tag.set(qn("w:val"), "single")
        tag.set(qn("w:sz"), size)
        tag.set(qn("w:color"), color)


def keep_with_next(paragraph):
    paragraph.paragraph_format.keep_with_next = True


def add_body(doc, text, first_indent=True, space_after=7):
    p = doc.add_paragraph()
    p.paragraph_format.line_spacing_rule = WD_LINE_SPACING.ONE_POINT_FIVE
    p.paragraph_format.space_after = Pt(space_after)
    if first_indent:
        p.paragraph_format.first_line_indent = Pt(21)
    p.alignment = WD_ALIGN_PARAGRAPH.JUSTIFY
    run = p.add_run(text)
    set_run_font(run, size=10.5)
    return p


def add_bullet(doc, text):
    p = doc.add_paragraph(style="List Bullet")
    p.paragraph_format.left_indent = Cm(0.75)
    p.paragraph_format.first_line_indent = Cm(-0.35)
    p.paragraph_format.line_spacing = 1.35
    p.paragraph_format.space_after = Pt(4)
    run = p.add_run(text)
    set_run_font(run, size=10.5)
    return p


def add_heading(doc, text, level=1):
    p = doc.add_paragraph(style=f"Heading {level}")
    p.paragraph_format.space_before = Pt(13 if level == 1 else 9)
    p.paragraph_format.space_after = Pt(7 if level == 1 else 5)
    p.paragraph_format.keep_with_next = True
    run = p.add_run(text)
    set_run_font(run, east="黑体", size=15 if level == 1 else 12, bold=True)
    return p


def add_table(doc, headers, rows, widths=None):
    table = doc.add_table(rows=1, cols=len(headers))
    table.alignment = WD_TABLE_ALIGNMENT.CENTER
    table.autofit = False
    set_table_borders(table)
    header = table.rows[0]
    header._tr.get_or_add_trPr().append(OxmlElement("w:tblHeader"))
    for idx, value in enumerate(headers):
        cell = header.cells[idx]
        cell.vertical_alignment = WD_CELL_VERTICAL_ALIGNMENT.CENTER
        set_cell_shading(cell, NAVY)
        set_cell_margins(cell)
        if widths:
            cell.width = Cm(widths[idx])
        p = cell.paragraphs[0]
        p.alignment = WD_ALIGN_PARAGRAPH.CENTER
        r = p.add_run(value)
        set_run_font(r, east="黑体", size=9.5, bold=True, color="FFFFFF")
    for row_index, values in enumerate(rows):
        cells = table.add_row().cells
        for idx, value in enumerate(values):
            cell = cells[idx]
            cell.vertical_alignment = WD_CELL_VERTICAL_ALIGNMENT.CENTER
            set_cell_margins(cell)
            if widths:
                cell.width = Cm(widths[idx])
            if row_index % 2:
                set_cell_shading(cell, PALE_GRAY)
            p = cell.paragraphs[0]
            p.paragraph_format.line_spacing = 1.2
            p.paragraph_format.space_after = Pt(0)
            p.alignment = WD_ALIGN_PARAGRAPH.CENTER if idx == 0 else WD_ALIGN_PARAGRAPH.LEFT
            r = p.add_run(str(value))
            set_run_font(r, size=9)
    doc.add_paragraph().paragraph_format.space_after = Pt(1)
    return table


def add_page_number(paragraph):
    paragraph.alignment = WD_ALIGN_PARAGRAPH.CENTER
    run = paragraph.add_run()
    fld_char1 = OxmlElement("w:fldChar")
    fld_char1.set(qn("w:fldCharType"), "begin")
    instr = OxmlElement("w:instrText")
    instr.set(qn("xml:space"), "preserve")
    instr.text = " PAGE "
    fld_char2 = OxmlElement("w:fldChar")
    fld_char2.set(qn("w:fldCharType"), "end")
    run._r.extend([fld_char1, instr, fld_char2])
    set_run_font(run, size=9, color="666666")


def configure_styles(doc):
    styles = doc.styles
    normal = styles["Normal"]
    normal.font.name = "Arial"
    normal._element.rPr.rFonts.set(qn("w:eastAsia"), "宋体")
    normal.font.size = Pt(10.5)
    for level in (1, 2):
        style = styles[f"Heading {level}"]
        style.font.color.rgb = RGBColor(0, 0, 0)
        style.font.name = "Arial"
        style._element.rPr.rFonts.set(qn("w:eastAsia"), "黑体")
    title = styles["Title"]
    title.font.color.rgb = RGBColor(0, 0, 0)
    title.font.name = "Arial"
    title._element.rPr.rFonts.set(qn("w:eastAsia"), "黑体")
    title_ppr = title._element.get_or_add_pPr()
    title_border = title_ppr.find(qn("w:pBdr"))
    if title_border is not None:
        title_ppr.remove(title_border)


def build_document():
    doc = Document()
    configure_styles(doc)
    section = doc.sections[0]
    section.top_margin = Cm(2.4)
    section.bottom_margin = Cm(2.2)
    section.left_margin = Cm(2.5)
    section.right_margin = Cm(2.5)
    section.header_distance = Cm(1.2)
    section.footer_distance = Cm(1.2)
    section.different_first_page_header_footer = True

    # Cover
    for _ in range(5):
        doc.add_paragraph()
    title = doc.add_paragraph(style="Title")
    title.alignment = WD_ALIGN_PARAGRAPH.CENTER
    title.paragraph_format.space_after = Pt(22)
    r = title.add_run("基于ESP32开发的在线电子阅读器")
    set_run_font(r, east="黑体", size=24, bold=True)
    subtitle = doc.add_paragraph()
    subtitle.alignment = WD_ALIGN_PARAGRAPH.CENTER
    subtitle.paragraph_format.space_after = Pt(70)
    r = subtitle.add_run("产业赛道项目计划书")
    set_run_font(r, east="黑体", size=18)
    meta = doc.add_paragraph()
    meta.alignment = WD_ALIGN_PARAGRAPH.CENTER
    meta.paragraph_format.line_spacing = 1.8
    r = meta.add_run("项目阶段  创意计划阶段\n申报方向  学校科技成果转化\n文档版本  申报稿")
    set_run_font(r, size=12)
    date = doc.add_paragraph()
    date.alignment = WD_ALIGN_PARAGRAPH.CENTER
    date.paragraph_format.space_before = Pt(78)
    r = date.add_run("二〇二六年九月")
    set_run_font(r, size=12)
    doc.add_page_break()

    # Opening summary
    add_heading(doc, "项目摘要", 1)
    add_body(doc, "本项目拟打造一款低成本、低干扰、可持续升级的便携式在线电子阅读器。产品基于低功耗微控制器、触摸彩色屏幕和无线网络能力，在资源有限的嵌入式设备上实现搜书、中文拼音输入、在线阅读、最近阅读书架、断点续读、亮度调节和触摸翻页。项目已完成可运行原型、在线内容获取、五章滑动缓存、多书籍切换和真机烧录验证，具备从课程作品继续迭代为校园科技成果和小批量产品的基础。")
    add_body(doc, "产品聚焦希望减少手机干扰、控制购置成本并获得专注阅读体验的学生、通勤人群和网络文学读者。系统优先下载当前章节，章节可读后立即进入阅读，其余内容在后台缓存；本地只保留阅读位置附近的章节，因此能够在有限存储条件下持续阅读长篇小说。通过开放的软件架构，后续可扩展多书源、本地书籍导入、护眼主题、字号调整、阅读统计和存储卡等功能。")

    add_heading(doc, "项目基本信息", 2)
    add_table(doc, ["项目要素", "内容"], [
        ["项目名称", "基于ESP32开发的在线电子阅读器"],
        ["所属领域", "文化体育和娱乐业"],
        ["项目定位", "低成本便携式专用阅读终端"],
        ["当前阶段", "功能原型已完成并通过真机启动与联网验证"],
        ["目标用户", "学生 通勤人群 网络文学爱好者 专注阅读用户"],
        ["核心价值", "减少手机干扰 降低设备门槛 提供连续在线阅读体验"],
    ], [3.4, 12.6])

    add_heading(doc, "一 项目背景与需求", 1)
    add_heading(doc, "一 阅读需求与使用痛点", 2)
    add_body(doc, "移动互联网已经使电子阅读更加便利，但手机同时承载社交、娱乐和消息通知，用户在阅读过程中容易被其他应用打断。专业电子阅读设备能够提供更专注的体验，但部分产品价格较高，网络文学获取方式和功能定制也受到限制。对于学生和入门用户而言，市场需要一种成本可控、操作简单、功能专注且能够持续升级的小型阅读终端。")
    add_heading(doc, "二 嵌入式设备面临的技术限制", 2)
    add_body(doc, "传统网页和移动应用通常依赖较大的运行内存与存储空间，而经典微控制器的资源较为有限。若一次性下载整本目录和正文，容易造成内存不足、网络超时与闪存占满。本项目通过渐进式目录发现、单页请求、章节临时文件、原子发布和动态缓存解决上述问题，使在线阅读能够稳定运行在小型硬件上。")

    add_heading(doc, "二 产品方案", 1)
    add_heading(doc, "一 产品形态", 2)
    add_body(doc, "产品由触摸显示终端和嵌入式阅读软件组成。用户开机连接无线网络后进入选书页面，可通过中文拼音搜索书籍，也可从最近阅读书架继续阅读。进入阅读器后，用户通过左右区域翻页，可返回书架切换书籍，也可调节屏幕亮度。设备不依赖手机应用即可完成主要阅读流程。")
    add_heading(doc, "二 核心功能", 2)
    add_table(doc, ["功能模块", "实现效果", "用户价值"], [
        ["书籍搜索", "按书名或作者搜索并展示候选结果", "扩大可阅读内容范围"],
        ["中文输入", "全键盘拼音输入及多候选字显示", "降低小屏输入门槛"],
        ["在线阅读", "当前章完成后立即进入阅读", "缩短首次等待时间"],
        ["动态缓存", "围绕阅读位置保留约五章内容", "支持长篇阅读并控制存储"],
        ["最近书架", "记录最近阅读书籍及章节进度", "便于快速续读与切换"],
        ["阅读控制", "触摸翻页 亮度调节 页面状态显示", "提升日常阅读舒适度"],
        ["防误触", "不足规定时长的触摸不触发操作", "减少误翻页和误点击"],
    ], [3.1, 7.0, 5.9])

    add_heading(doc, "三 典型使用流程", 2)
    add_table(doc, ["步骤", "用户操作", "系统响应"], [
        ["一", "开机并连接无线网络", "读取已保存配置并进入选书页面"],
        ["二", "搜索书名或选择书架记录", "获取书籍信息并恢复阅读位置"],
        ["三", "等待当前章节", "优先下载当前章 完成后立即进入阅读"],
        ["四", "翻页或切换章节", "展示正文并在后台缓存相邻章节"],
        ["五", "退出或切换书籍", "保存进度并安全切换后台任务"],
    ], [2.0, 6.0, 8.0])

    add_heading(doc, "三 技术实现", 1)
    add_heading(doc, "一 总体架构", 2)
    add_body(doc, "系统采用分层设计。硬件层负责显示屏、触摸控制器、无线网络和背光调节；系统层负责任务调度、网络连接和持久化存储；数据层负责书籍搜索、目录发现、网页解析、章节缓存和阅读进度；界面层负责选书、书架、键盘、阅读器和亮度控制。界面任务与网络下载任务相互分离，避免网络等待阻塞触摸和页面刷新。")
    add_table(doc, ["层级", "主要组成", "职责"], [
        ["界面层", "选书页 书架 拼音键盘 阅读页 亮度面板", "显示状态并处理用户交互"],
        ["业务层", "阅读状态 书籍切换 章节调度 进度管理", "协调阅读流程与后台任务"],
        ["数据层", "搜索解析 目录索引 章节缓存 本地文件", "获取 清洗 保存和读取内容"],
        ["系统层", "实时任务调度 网络协议 非易失存储 文件系统", "提供并发 网络和持久化能力"],
        ["硬件层", "微控制器 彩色屏幕 触摸控制器 背光", "提供计算 显示 触摸和联网基础"],
    ], [2.4, 7.2, 6.4])

    add_heading(doc, "二 渐进式目录与分页抓取", 2)
    add_body(doc, "为避免一次性获取完整目录造成超时或内存压力，系统按阅读需求逐页发现章节。单个章节若在来源网站上被拆分为多个网页，系统会按页面依次请求并追加到临时文件，每页处理完成后立即释放内存。只有全部分页成功后才发布正式章节文件，从而避免断网后留下不完整正文。")
    add_heading(doc, "三 五章滑动缓存", 2)
    add_body(doc, "缓存窗口随阅读位置移动。首次进入时优先缓存前五章；阅读位置向后推进后，系统清理远离当前位置的旧章节，并补充新的相邻章节。当前章节拥有最高下载优先级，完成后再处理附近章节。该机制使本地正文容量保持稳定，同时章节索引仍可持续增长至小说结尾。")
    add_heading(doc, "四 数据与可靠性", 2)
    add_bullet(doc, "章节正文和每本书的章节索引保存在本地文件系统中")
    add_bullet(doc, "无线网络配置 屏幕亮度 最近书架和阅读进度保存在非易失存储中")
    add_bullet(doc, "网络失败时进行重试并在可用来源之间切换")
    add_bullet(doc, "书籍切换时先安全停止旧下载任务 防止并发修改缓存")
    add_bullet(doc, "触摸输入在底层经过时长确认 短暂接触不会产生点击事件")

    add_heading(doc, "四 项目创新点", 1)
    add_table(doc, ["创新方向", "具体做法", "形成的效果"], [
        ["资源受限在线阅读", "以渐进获取替代整本目录和正文一次性载入", "经典微控制器也能持续阅读长篇内容"],
        ["首章快速进入", "当前章节优先 其余章节后台加载", "减少用户等待并保持阅读连续性"],
        ["滑动章节缓存", "缓存窗口随阅读位置动态移动", "平衡存储占用与下一章可用性"],
        ["多书籍轻量索引", "不同书籍独立保存章节编号和阅读位置", "支持书架续读且不长期保存整本正文"],
        ["嵌入式中文搜索", "全键盘拼音 轻量词库 候选字显示", "在小屏终端完成中文书名输入"],
        ["统一防误触", "在输入驱动层设置按压确认门槛", "覆盖翻页 按钮 键盘和候选字"],
    ], [3.2, 7.1, 5.9])

    add_heading(doc, "五 用户与市场分析", 1)
    add_heading(doc, "一 目标用户", 2)
    add_body(doc, "首批目标用户为高校学生和网络文学阅读者，重点覆盖希望降低手机干扰、需要通勤阅读、关注硬件价格或喜欢开源硬件的群体。第二阶段可面向中小学生家庭、图书馆阅读活动、创客教育课程和企业礼品定制等场景拓展。")
    add_heading(doc, "二 替代方案比较", 2)
    add_table(doc, ["方案", "优势", "不足", "本项目切入点"], [
        ["手机阅读软件", "内容丰富 使用方便", "消息与娱乐干扰较多", "提供专注且独立的阅读终端"],
        ["专业电子阅读器", "屏幕舒适 生态成熟", "购买成本和定制门槛较高", "强调低成本和开放扩展"],
        ["普通开发板阅读器", "适合学习和实验", "常缺少完整搜书与缓存闭环", "形成可实际连续使用的软件系统"],
        ["本项目", "便携 低成本 可联网 可升级", "当前仍需持续完善外壳与内容来源", "从校园原型逐步转化为小批量产品"],
    ], [2.7, 4.2, 4.2, 4.9])

    add_heading(doc, "六 成果转化与商业模式", 1)
    add_heading(doc, "一 成果形态", 2)
    add_body(doc, "项目可形成嵌入式阅读器固件、中文输入与章节缓存软件模块、硬件装配方案、产品交互设计和使用文档等成果。在完成稳定性测试后，可进一步整理软件著作权材料、外观结构方案和标准化烧录工具，为学校科技成果登记及后续合作奠定基础。")
    add_heading(doc, "二 转化路径", 2)
    add_table(doc, ["阶段", "主要任务", "预期成果"], [
        ["原型完善", "修复体验问题 完成连续阅读和异常测试", "可稳定演示和试用的工程样机"],
        ["校园试用", "组织小规模用户测试 收集阅读时长和问题反馈", "需求清单与产品迭代报告"],
        ["产品化设计", "优化外壳 电源管理 操作说明和量产烧录", "小批量试制版本"],
        ["成果登记", "整理技术文档 代码说明和权属材料", "软件著作权或校级成果材料"],
        ["合作推广", "与创客教育 硬件社群和校园渠道合作", "教学套件 定制终端或授权方案"],
    ], [2.7, 8.0, 5.3])

    add_heading(doc, "三 商业模式设想", 2)
    add_body(doc, "项目早期以硬件套件和校园定制为主要方向，通过整机销售、教学套件、定制固件和技术服务获得收入。成熟后可将软件模块授权给同类硬件厂商，也可面向学校阅读活动提供批量设备与管理支持。内容获取功能应坚持合法合规，商业化版本将优先接入获得授权的内容接口或由用户导入自有书籍。")

    add_heading(doc, "七 实施计划", 1)
    add_table(doc, ["时间阶段", "工作重点", "验收标准"], [
        ["近期", "完善触摸体验 中文输入 网络异常处理和代码文档", "核心流程无阻塞 可稳定连续阅读"],
        ["第一阶段", "开展校园用户试用并记录操作问题", "形成结构化反馈和优先级清单"],
        ["第二阶段", "增加本地导入 字号主题 电量与休眠管理", "满足日常便携阅读需求"],
        ["第三阶段", "设计外壳和装配流程 完成小批量试制", "形成可复制的样机生产方案"],
        ["后续阶段", "推动成果登记 内容合规合作和渠道验证", "具备成果转化或合作落地条件"],
    ], [2.8, 8.2, 5.0])

    add_heading(doc, "八 团队组织与分工计划", 1)
    add_body(doc, "项目采用小型跨职能团队方式推进。技术负责人统筹嵌入式软件、网络解析和系统稳定性；产品负责人负责用户需求、交互流程和版本规划；硬件负责人负责电源、结构、装配和可靠性；运营负责人负责校园试用、用户反馈、成果材料和合作沟通。当前未确定的岗位可通过校内招募和指导教师协作逐步补充。")
    add_table(doc, ["工作角色", "主要职责", "阶段产出"], [
        ["项目统筹", "计划管理 资源协调 申报与验收", "计划书 阶段报告 验收材料"],
        ["软件开发", "界面 网络 缓存 存储和系统优化", "固件源码 可烧录程序 技术文档"],
        ["硬件设计", "供电 结构 装配和样机测试", "硬件清单 装配规范 工程样机"],
        ["产品与测试", "需求分析 可用性测试 缺陷管理", "测试用例 用户反馈 迭代清单"],
        ["成果与运营", "权属材料 校园试用 合作沟通", "成果登记材料 试用报告 合作方案"],
    ], [3.0, 7.3, 5.7])

    add_heading(doc, "九 风险分析与应对", 1)
    add_table(doc, ["风险", "可能影响", "应对措施"], [
        ["内容来源变化", "搜索或章节获取失效", "采用适配层设计 增加合规内容接口和本地导入"],
        ["网络环境不稳定", "章节加载时间增加", "失败重试 备用来源 当前章优先和本地缓存"],
        ["硬件资源有限", "界面卡顿或内存不足", "限制缓存窗口 逐页处理 数据流式写入"],
        ["触摸精度不足", "误翻页和误操作", "按压时长保护 触摸校准和交互区域优化"],
        ["成果权属不清", "影响登记与合作", "保留开发记录 明确成员贡献并按学校制度确认权属"],
        ["商业化合规风险", "内容服务无法持续", "商业版本仅使用授权接口或用户自有内容"],
    ], [3.0, 5.2, 7.8])

    doc.add_page_break()
    add_heading(doc, "十 当前成果与验收指标", 1)
    add_heading(doc, "一 已完成成果", 2)
    add_bullet(doc, "完成触摸彩屏阅读器的基础硬件适配和图形界面")
    add_bullet(doc, "完成无线联网 书籍搜索 中文拼音输入和最近书架")
    add_bullet(doc, "完成章节逐页抓取 正文解析 五章滑动缓存和断点续读")
    add_bullet(doc, "完成亮度调节 按压防误触 网络重试和多来源切换")
    add_bullet(doc, "完成固件编译 真机烧录 哈希校验和串口启动验证")

    add_heading(doc, "二 下一阶段验收指标", 2)
    add_table(doc, ["指标类别", "目标"], [
        ["功能完整性", "搜书 选书 阅读 缓存 续读和亮度调节形成完整闭环"],
        ["稳定性", "连续阅读和切换书籍过程中不出现崩溃及进度丢失"],
        ["响应体验", "当前章节就绪后立即进入阅读 后台任务不阻塞界面"],
        ["存储控制", "正文缓存保持在设定窗口范围并可持续扩展章节索引"],
        ["可维护性", "主要模块接口清晰 具备构建说明和测试记录"],
        ["转化准备", "形成样机 技术文档 用户反馈和成果登记基础材料"],
    ], [3.6, 12.4])

    add_heading(doc, "结语", 1)
    add_body(doc, "本项目以真实可运行的嵌入式阅读原型为基础，围绕低成本、低干扰和可持续阅读三个目标，完成了从联网搜书到动态缓存和断点续读的核心闭环。下一阶段将以稳定性、内容合规、用户试用和产品化设计为重点，逐步推动项目由学生创新原型向可登记、可试制、可合作的学校科技成果转化。")

    # Header and footer on non-cover pages. Word uses the same section, so keep
    # the header subtle enough for the cover as well.
    header_p = section.header.paragraphs[0]
    header_p.alignment = WD_ALIGN_PARAGRAPH.RIGHT
    r = header_p.add_run("在线电子阅读器项目计划书")
    set_run_font(r, east="宋体", size=8.5, color="777777")
    add_page_number(section.footer.paragraphs[0])
    section.first_page_header.paragraphs[0].clear()
    section.first_page_footer.paragraphs[0].clear()

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    doc.save(OUT_PATH)
    print(OUT_PATH)


if __name__ == "__main__":
    build_document()
